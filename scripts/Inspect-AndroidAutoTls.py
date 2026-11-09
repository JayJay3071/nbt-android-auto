"""Generate a fresh TLS 1.2 ClientHello or inspect an unauthenticated USB capture.

This diagnostic never completes a handshake, accepts a peer, or emits private
keys. The saved certificates are untrusted evidence, not a trust store.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import ssl
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / '.local/python-packages'))


def client_hello():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    # No DNS identity has been established for AA. Omit SNI for this capture;
    # no certificate is accepted and no handshake is completed here.
    ctx.check_hostname = False
    assert ctx.verify_mode == ssl.CERT_REQUIRED
    ctx.set_ciphers('ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:AES128-GCM-SHA256:AES128-SHA256')
    outgoing = ssl.MemoryBIO()
    engine = ctx.wrap_bio(ssl.MemoryBIO(), outgoing, server_side=False)
    try:
        engine.do_handshake()
    except ssl.SSLWantReadError:
        pass
    else:
        raise RuntimeError('Unexpected completed handshake')
    data = outgoing.read()
    if not (44 <= len(data) <= 4096 and data[0] == 22 and data[5] == 1
            and int.from_bytes(data[3:5], 'big') == len(data) - 5):
        raise RuntimeError('Unexpected ClientHello encoding')
    return data


def parse_capture(raw):
    """Parse complete AA frames, TLS records and handshake messages strictly."""
    tls = bytearray()
    while raw:
        if len(raw) < 4:
            raise ValueError('Incomplete AA header')
        size = int.from_bytes(raw[2:4], 'big')
        if raw[:2] != b'\0\3' or size < 2 or len(raw) < size + 4:
            raise ValueError('Incomplete or unsupported fragmented AA frame')
        if raw[4:6] != b'\0\3':
            raise ValueError('Unexpected AA message; expected encapsulated TLS')
        tls.extend(raw[6:4 + size])
        raw = raw[4 + size:]
    messages = bytearray()
    alerts = []
    while tls:
        if len(tls) < 5:
            raise ValueError('Incomplete TLS record header')
        size = int.from_bytes(tls[3:5], 'big')
        if tls[1] != 3 or size > 18432 or len(tls) < size + 5:
            raise ValueError('Incomplete or oversized TLS record')
        payload = tls[5:5 + size]
        if tls[0] == 22:
            messages.extend(payload)
        elif tls[0] == 21:
            if len(payload) != 2:
                raise ValueError('Invalid TLS alert')
            alerts.append(list(payload))
        else:
            raise ValueError('Unexpected TLS record type before authentication')
        del tls[:5 + size]
    certs, kinds, server = [], [], None
    while messages:
        if len(messages) < 4:
            raise ValueError('Incomplete TLS handshake header')
        kind = messages[0]
        size = int.from_bytes(messages[1:4], 'big')
        if len(messages) < size + 4:
            raise ValueError('Incomplete TLS handshake message')
        payload = bytes(messages[4:4 + size])
        kinds.append(kind)
        if kind == 2:
            if len(payload) < 38:
                raise ValueError('Short ServerHello')
            pos = 35 + payload[34]
            if len(payload) < pos + 3:
                raise ValueError('Short ServerHello session/cipher')
            server = {'version': payload[:2].hex(), 'cipher': payload[pos:pos + 2].hex()}
        elif kind == 11:
            if len(payload) < 3 or int.from_bytes(payload[:3], 'big') != len(payload) - 3:
                raise ValueError('Invalid certificate list length')
            payload = payload[3:]
            while payload:
                if len(payload) < 3:
                    raise ValueError('Short certificate length')
                n = int.from_bytes(payload[:3], 'big')
                if not n or n > len(payload) - 3 or len(certs) >= 16:
                    raise ValueError('Invalid certificate size/count')
                certs.append(payload[3:3 + n])
                payload = payload[3 + n:]
        del messages[:4 + size]
    return {'authenticated': False, 'server_hello': server, 'message_types': kinds,
            'server_hello_done': 14 in kinds, 'alerts': alerts}, certs


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='action', required=True)
    h = sub.add_parser('hello')
    h.add_argument('output', type=Path)
    c = sub.add_parser('capture')
    c.add_argument('log', type=Path)
    c.add_argument('output_dir', type=Path)
    args = p.parse_args()
    if args.action == 'hello':
        data = client_hello()
        with args.output.open('xb') as f:
            f.write(data)
        print(json.dumps({'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}))
        return
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    chunks = re.findall(r'^AA_TLS_PUBLIC_FLIGHT_HEX=([0-9a-f]+)$', args.log.read_text(), re.M)
    if not chunks:
        raise ValueError('No server flight captured')
    raw = bytes.fromhex(''.join(chunks))
    if len(raw) > 65536:
        raise ValueError('Capture exceeds limit')
    summary, certs = parse_capture(raw)
    args.output_dir.mkdir(exist_ok=False)
    summary['certificates'] = []
    for i, der in enumerate(certs):
        cert = x509.load_der_x509_certificate(der)
        (args.output_dir / ('untrusted-peer-%d.pem' % i)).write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        summary['certificates'].append({'sha256': cert.fingerprint(hashes.SHA256()).hex(),
            'subject': cert.subject.rfc4514_string(), 'issuer': cert.issuer.rfc4514_string(),
            'not_before': cert.not_valid_before_utc.isoformat(), 'not_after': cert.not_valid_after_utc.isoformat()})
    (args.output_dir / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
