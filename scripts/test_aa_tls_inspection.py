import importlib.util
from pathlib import Path
import unittest
import ssl
import tempfile

spec = importlib.util.spec_from_file_location('inspection', Path(__file__).with_name('Inspect-AndroidAutoTls.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def handshake(kind, data):
    return bytes([kind]) + len(data).to_bytes(3, 'big') + data


def record(data):
    return b'\x16\x03\x03' + len(data).to_bytes(2, 'big') + data


def frame(data):
    return b'\0\3' + (len(data) + 2).to_bytes(2, 'big') + b'\0\3' + data


class InspectionTests(unittest.TestCase):
    def test_real_tls_server_certificate_flight(self):
        fixture_spec = importlib.util.spec_from_file_location('tls_fixtures', Path(__file__).with_name('Test-QnxTls.py'))
        fixtures = importlib.util.module_from_spec(fixture_spec)
        fixture_spec.loader.exec_module(fixtures)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, data in fixtures.fixtures().items():
                (root / name).write_bytes(data)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(root / 'lab-server.pem', root / 'lab-server-key.pem')
            incoming, outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
            server = context.wrap_bio(incoming, outgoing, server_side=True)
            incoming.write(m.client_hello())
            with self.assertRaises(ssl.SSLWantReadError): server.do_handshake()
            result, certs = m.parse_capture(frame(outgoing.read()))
            self.assertTrue(result['server_hello_done'])
            self.assertFalse(result['authenticated'])
            self.assertEqual(len(certs), 1)
            from cryptography import x509
            self.assertIn('qnx-aa-lab.local', x509.load_der_x509_certificate(certs[0]).subject.rfc4514_string())

    def test_fresh_hello(self):
        a, b = m.client_hello(), m.client_hello()
        self.assertNotEqual(a[11:43], b[11:43])
        self.assertEqual(a[9:11], b'\3\3')

    def test_split_records_and_coalesced_frames(self):
        hello = b'\3\3' + bytes(32) + b'\0\xc0\x2f\0'
        cert = b'fake DER for framing test'
        entry = len(cert).to_bytes(3, 'big') + cert
        flight = handshake(2, hello) + handshake(11, len(entry).to_bytes(3, 'big') + entry) + handshake(14, b'')
        wire = record(flight[:17]) + record(flight[17:])
        result, certs = m.parse_capture(frame(wire[:8]) + frame(wire[8:]))
        self.assertEqual(result['server_hello']['cipher'], 'c02f')
        self.assertTrue(result['server_hello_done'])
        self.assertFalse(result['authenticated'])
        self.assertEqual(certs, [cert])
        for n in range(1, len(wire)):
            # Some boundaries end a complete TLS message; only explicitly
            # incomplete headers/records must reject, not every prefix.
            if n < 5:
                with self.assertRaises(ValueError): m.parse_capture(frame(wire[:n]))

    def test_reject_invalid_lengths_types_and_fragments(self):
        for raw in [b'\0', b'\0\3\0\4\0\3', b'\0\1\0\2\0\3',
                    frame(b'\x17\3\3\0\0'), frame(record(handshake(11, b'\0\0\5x'))),
                    frame(record(b'\x0b\0\0\x08x'))]:
            with self.assertRaises(ValueError): m.parse_capture(raw)


if __name__ == '__main__':
    unittest.main()
