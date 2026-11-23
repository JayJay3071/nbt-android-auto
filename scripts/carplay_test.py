"""Host test of nbt_carplay's AirPlay receiver: plays the iPhone's side against test_airplay_host on 127.0.0.1:7000.

SPDX-License-Identifier: GPL-3.0-only
Copyright (C) 2026 Valentin Radu

Covers what the car cannot be asked for while it is off: /info, pair-setup (SRP-6a, Ed25519), pair-verify (X25519),
the encrypted control connection, auth-setup (with test_airplay_host's fake chip), SETUP of the session and of a
screen and an audio stream, the screen stream (configuration + encrypted frames -> Annex B in the dump file), the
audio stream (encrypted RTP -> WAV), the event connection (a key line in the input FIFO -> HID report), timing,
feedback and TEARDOWN. Needs Python 3.8+ with "cryptography".

  python3 scripts/carplay_test.py DUMP_FILE INPUT_FIFO AUDIO_DIR
"""
import hashlib
import os
import plistlib
import socket
import struct
import sys
import threading
import time

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.hashes import SHA512
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

N = int("FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD"
        "3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F"
        "24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F35620855"
        "2BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF"
        "06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64E"
        "CFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98"
        "A0864D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D"
        "120A93AD2CAFFFFFFFFFFFFFFFF", 16)
G = 5
NB = 384


def H(*parts):
    h = hashlib.sha512()
    for p in parts:
        h.update(p)
    return h.digest()


def pad(x):
    return x.to_bytes(NB, "big")


def tob(x):
    return x.to_bytes((x.bit_length() + 7) // 8 or 1, "big")


def hkdf(ikm, salt, info, n=32):
    return HKDF(SHA512(), n, salt.encode(), info.encode()).derive(ikm)


def nonce_label(label):
    return b"\0" * 4 + label.encode()


def nonce_counter(c):
    return b"\0" * 4 + struct.pack("<Q", c)


def tlv(*items):
    out = b""
    for t, v in items:
        if not v:
            out += bytes([t, 0])
        while v:
            out += bytes([t, min(255, len(v))]) + v[:255]
            v = v[255:]
    return out


def untlv(b):
    out, i, last, lastlen = {}, 0, None, 0
    while i + 2 <= len(b):
        t, l = b[i], b[i + 1]
        v = b[i + 2:i + 2 + l]
        if t == last and lastlen == 255:
            out[t] += v
        else:
            out[t] = v
        last, lastlen = t, l
        i += 2 + l
    return out


class Cipher2:
    def __init__(self, read_key, write_key):
        self.r, self.w = ChaCha20Poly1305(read_key), ChaCha20Poly1305(write_key)
        self.rn = self.wn = 0
        self.buf = b""

    def seal(self, data):
        out = b""
        while True:
            chunk, data = data[:0x4000], data[0x4000:]
            head = struct.pack("<H", len(chunk))
            out += head + self.w.encrypt(nonce_counter(self.wn), chunk, head)
            self.wn += 1
            if not data:
                return out

    def open(self, data):
        self.buf += data
        out = b""
        while len(self.buf) >= 2:
            n = struct.unpack("<H", self.buf[:2])[0]
            if len(self.buf) < 2 + n + 16:
                break
            out += self.r.decrypt(nonce_counter(self.rn), self.buf[2:2 + n + 16], self.buf[:2])
            self.rn += 1
            self.buf = self.buf[2 + n + 16:]
        return out


class Rtsp:
    def __init__(self, sock):
        self.s, self.cipher, self.cseq, self.plain = sock, None, 0, b""

    def recv_message(self):
        while True:
            end = self.plain.find(b"\r\n\r\n")
            if end >= 0:
                head = self.plain[:end].decode()
                length = 0
                for line in head.split("\r\n")[1:]:
                    k, _, v = line.partition(":")
                    if k.strip().lower() == "content-length":
                        length = int(v)
                if len(self.plain) >= end + 4 + length:
                    body = self.plain[end + 4:end + 4 + length]
                    self.plain = self.plain[end + 4 + length:]
                    return head, body
            data = self.s.recv(65536)
            if not data:
                raise EOFError("closed")
            self.plain += self.cipher.open(data) if self.cipher else data

    def request(self, method, path, body=b"", ctype="application/x-apple-binary-plist"):
        self.cseq += 1
        head = "%s %s RTSP/1.0\r\nCSeq: %d\r\nContent-Length: %d\r\n" % (method, path, self.cseq, len(body))
        if body:
            head += "Content-Type: %s\r\n" % ctype
        wire = head.encode() + b"\r\n" + body
        self.s.sendall(self.cipher.seal(wire) if self.cipher else wire)
        head, body = self.recv_message()
        status = int(head.split(" ")[1])
        assert ("CSeq: %d" % self.cseq) in head, head
        return status, body


def check(what, cond):
    print(("OK   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        sys.exit(1)


def main():
    dump, input_fifo, audio_dir = sys.argv[1:4]
    s = socket.create_connection(("127.0.0.1", 7000))
    r = Rtsp(s)

    status, body = r.request("GET", "/info")
    info = plistlib.loads(body)
    disp = info["displays"][0]
    check("/info: 1280x480 display, knob", status == 200 and disp["widthPixels"] == 1280 and
          disp["heightPixels"] == 480 and disp["features"] == 2 and len(info["hidDevices"]) == 3)
    check("/info: audio formats without type 102", all(f["type"] != 102 for f in info["audioFormats"]))
    check("/info: speechMode -1 as a real (xcertplay)", info["modes"]["appStates"][1]["speechMode"] == -1)

    # pair-setup
    status, body = r.request("POST", "/pair-setup", tlv((0, b"\0"), (6, b"\1")), "application/octet-stream")
    m2 = untlv(body)
    check("pair-setup M2", status == 200 and m2[6] == b"\2" and len(m2[3]) == NB and len(m2[2]) == 16)
    salt, B = m2[2], int.from_bytes(m2[3], "big")
    user, password = b"Pair-Setup", b"3939"
    a = int.from_bytes(os.urandom(32), "big")
    A = pow(G, a, N)
    k = int.from_bytes(H(tob(N), pad(G)), "big")
    u = int.from_bytes(H(pad(A), pad(B)), "big")
    x = int.from_bytes(H(salt, H(user + b":" + password)), "big")
    S = pow((B - k * pow(G, x, N)) % N, a + u * x, N)
    K = H(tob(S))
    hn, hg = H(tob(N)), H(tob(G))
    M1 = H(bytes(i ^ j for i, j in zip(hn, hg)), H(user), salt, pad(A), pad(B), K)
    status, body = r.request("POST", "/pair-setup", tlv((6, b"\3"), (3, pad(A)), (4, M1)), "application/octet-stream")
    m4 = untlv(body)
    check("pair-setup M4: server proof", m4.get(6) == b"\4" and m4.get(4) == H(pad(A), M1, K))
    ltsk = Ed25519PrivateKey.generate()
    ltpk = ltsk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    cid = b"8F3A1C55-TEST-PHONE"
    enc_key = hkdf(K, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info")
    sign = ltsk.sign(hkdf(K, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info") + cid + ltpk)
    sub = tlv((1, cid), (3, ltpk), (10, sign))
    sealed = ChaCha20Poly1305(enc_key).encrypt(nonce_label("PS-Msg05"), sub, None)
    status, body = r.request("POST", "/pair-setup", tlv((6, b"\5"), (5, sealed)), "application/octet-stream")
    m6 = untlv(body)
    check("pair-setup M6", m6.get(6) == b"\6")
    sub = untlv(ChaCha20Poly1305(enc_key).decrypt(nonce_label("PS-Msg06"), m6[5], None))
    acc_id, acc_pk = sub[1], sub[3]
    Ed25519PublicKey.from_public_bytes(acc_pk).verify(
        sub[10], hkdf(K, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info") + acc_id + acc_pk)
    check("pair-setup M6: accessory signature (id %s)" % acc_id.decode(), True)

    # pair-verify
    eph = X25519PrivateKey.generate()
    eph_pub = eph.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    status, body = r.request("POST", "/pair-verify", tlv((6, b"\1"), (3, eph_pub)), "application/octet-stream")
    v2 = untlv(body)
    acc_eph = v2[3]
    shared = eph.exchange(X25519PublicKey.from_public_bytes(acc_eph))
    vkey = hkdf(shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info")
    sub = untlv(ChaCha20Poly1305(vkey).decrypt(nonce_label("PV-Msg02"), v2[5], None))
    Ed25519PublicKey.from_public_bytes(acc_pk).verify(sub[10], acc_eph + sub[1] + eph_pub)
    check("pair-verify M2: accessory signature", sub[1] == acc_id)
    sig = ltsk.sign(eph_pub + cid + acc_eph)
    sealed = ChaCha20Poly1305(vkey).encrypt(nonce_label("PV-Msg03"), tlv((1, cid), (10, sig)), None)
    status, body = r.request("POST", "/pair-verify", tlv((6, b"\3"), (5, sealed)), "application/octet-stream")
    check("pair-verify M4", untlv(body).get(6) == b"\4")
    r.cipher = Cipher2(hkdf(shared, "Control-Salt", "Control-Read-Encryption-Key"),
                       hkdf(shared, "Control-Salt", "Control-Write-Encryption-Key"))

    # auth-setup (fake chip: the signature is the digest, padded with a5)
    peer = X25519PrivateKey.generate()
    peer_pub = peer.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    status, body = r.request("POST", "/auth-setup", b"\1" + peer_pub, "application/octet-stream")
    their = body[:32]
    clen = struct.unpack(">I", body[32:36])[0]
    cert = body[36:36 + clen]
    slen = struct.unpack(">I", body[36 + clen:40 + clen])[0]
    enc = body[40 + clen:40 + clen + slen]
    sh = peer.exchange(X25519PublicKey.from_public_bytes(their))
    key = hashlib.sha1(b"AES-KEY" + sh).digest()[:16]
    iv = hashlib.sha1(b"AES-IV" + sh).digest()[:16]
    dec = Cipher(algorithms.AES(key), modes.CTR(iv)).decryptor().update(enc)
    check("auth-setup (encrypted, over the encrypted control connection)",
          status == 200 and cert == b"test certificate" and dec[:20] == hashlib.sha1(their + peer_pub).digest())

    # timing: answer the receiver's requests
    tsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tsock.bind(("127.0.0.1", 0))
    timing_seen = []

    def timing():
        while True:
            try:
                m, addr = tsock.recvfrom(128)
            except OSError:
                return
            if m[1] == 210:
                now = struct.pack(">II", int(time.time()) + 2208988800 + 5, 0)   # a phone 5 s ahead
                tsock.sendto(bytes([0x80, 211]) + m[2:8] + m[24:32] + now + now, addr)
                timing_seen.append(1)
    threading.Thread(target=timing, daemon=True).start()

    status, body = r.request("SETUP", "/1", plistlib.dumps(
        {"timingPort": tsock.getsockname()[1], "features": ["viewAreas", "iAPChannel"], "name": "Test iPhone",
         "model": "iPhone16,2", "deviceID": "aa:bb:cc:dd:ee:ff"}, fmt=plistlib.FMT_BINARY))
    st = plistlib.loads(body)
    check("SETUP session: event and timing ports, features %s" % st["enabledFeatures"],
          st["eventPort"] > 0 and st["timingPort"] > 0 and st["enabledFeatures"] == ["viewAreas"])
    ev = socket.create_connection(("127.0.0.1", st["eventPort"]))
    evc = Cipher2(hkdf(shared, "Events-Salt", "Events-Write-Encryption-Key"),
                  hkdf(shared, "Events-Salt", "Events-Read-Encryption-Key"))

    conn_screen, conn_audio = 0x1234567890abcdef, 0xfedcba9876543210   # the second with the top bit set
    status, body = r.request("SETUP", "/1", plistlib.dumps({"streams": [
        {"type": 110, "streamConnectionID": conn_screen},
        {"type": 100, "audioType": "media", "audioFormat": 0x800, "streamConnectionID": conn_audio - (1 << 64)},
        {"type": 102, "audioType": "media", "audioFormat": 0x400000, "streamConnectionID": 7}]},
        fmt=plistlib.FMT_BINARY))
    streams = plistlib.loads(body)["streams"]
    types = [x["type"] for x in streams]
    check("SETUP streams: screen and LPCM audio taken, AAC refused %s" % types, types == [110, 100])
    audio = streams[1]
    check("SETUP streams: 64-bit connection ID echoed unsigned", audio["streamConnectionID"] == conn_audio)
    status, _ = r.request("RECORD", "/1")
    check("RECORD", status == 200)

    # screen: configuration, then two frames
    sc = socket.create_connection(("127.0.0.1", streams[0]["dataPort"]))
    skey = hkdf(shared, "DataStream-Salt%d" % conn_screen, "DataStream-Output-Encryption-Key")
    sps, pps = bytes.fromhex("6764001facd9405005bb011000000300100000030320f1831960"), bytes.fromhex("68ebecb22c")
    avcc = bytes([1, 0x64, 0, 0x1f, 0xff, 0xe1]) + struct.pack(">H", len(sps)) + sps + b"\1" + \
        struct.pack(">H", len(pps)) + pps
    cfg = b"\0" * 16 + b"avcC" + avcc
    head = bytearray(128)
    struct.pack_into("<I", head, 0, len(cfg))
    head[4] = 1
    sc.sendall(bytes(head) + cfg)
    frames = [b"\x65" + os.urandom(300), b"\x41" + os.urandom(200)]
    for i, nal in enumerate(frames):
        payload = struct.pack(">I", len(nal)) + nal
        head = bytearray(128)
        struct.pack_into("<I", head, 0, len(payload) + 16)
        head[4] = 0
        sealed = ChaCha20Poly1305(skey).encrypt(nonce_counter(i), payload, bytes(head))
        sc.sendall(bytes(head) + sealed)

    # audio: 10 packets of 352 stereo frames, big-endian
    akey = hkdf(shared, "DataStream-Salt%d" % conn_audio, "DataStream-Output-Encryption-Key")
    asock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for i in range(10):
        samples = struct.pack(">" + "h" * 704, *([1000 * (i + 1), -1000 * (i + 1)] * 352))
        rtp = bytes([0x80, 0x60]) + struct.pack(">HII", i, 1000 + 352 * i, 0x1234)
        nonce8 = struct.pack("<Q", i)
        sealed = ChaCha20Poly1305(akey).encrypt(b"\0" * 4 + nonce8, samples, rtp[4:12])
        asock.sendto(rtp + sealed + nonce8, ("127.0.0.1", audio["dataPort"]))
    time.sleep(0.5)

    # the event connection: a knob turn from the input FIFO becomes a HID report
    with open(input_fifo, "w") as f:
        f.write("R 65536 1\nK 23 1\n")
    ev.settimeout(3)
    plain = b""
    reports = []
    while len(reports) < 3:
        plain += evc.open(ev.recv(4096))
        while b"\r\n\r\n" in plain:
            h, _, rest = plain.partition(b"\r\n\r\n")
            n = int([ln for ln in h.decode().split("\r\n") if ln.lower().startswith("content-length")][0].split(":")[1])
            if len(rest) < n:
                break
            cmd = plistlib.loads(rest[:n])
            plain = rest[n:]
            reports.append(cmd)
            reply = evc.seal(b"RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n")
            ev.sendall(reply)
    check("event: knob reports %s" % [x["hidReport"].hex() for x in reports],
          reports[0]["type"] == "hidSendReport" and reports[0]["uuid"] == "2a2a2a2b" and
          reports[0]["hidReport"] == b"\0\0\0\1" and reports[1]["hidReport"] == b"\0\0\0\0" and
          reports[2]["hidReport"] == b"\1\0\0\0")

    # an event request from the phone is answered
    body = plistlib.dumps({"type": "modesChanged", "params": {"appStates": [{"appStateID": 1, "speechMode": 1}]}},
                          fmt=plistlib.FMT_BINARY)
    ev.sendall(evc.seal(b"POST /command RTSP/1.0\r\nCSeq: 9\r\nContent-Length: %d\r\n\r\n" % len(body) + body))
    while b"\r\n\r\n" not in plain:
        plain += evc.open(ev.recv(4096))
    check("event: command answered", plain.startswith(b"RTSP/1.0 200 OK") and b"CSeq: 9" in plain)

    status, body = r.request("POST", "/feedback")
    fb = plistlib.loads(body)["streams"][0]
    check("feedback: audio stream clock %s" % {k: fb[k] for k in ("type", "sampleRate", "sampleTime")},
          status == 200 and fb["type"] == 100 and fb["sampleRate"] == 44100 and fb["streamConnectionID"] == conn_audio)
    time.sleep(2.5)
    check("timing: the receiver asked for the time (%d)" % len(timing_seen), len(timing_seen) >= 2)

    status, _ = r.request("TEARDOWN", "/1", plistlib.dumps({"streams": [{"type": 110}, {"type": 100}]},
                                                             fmt=plistlib.FMT_BINARY))
    check("TEARDOWN", status == 200)
    time.sleep(0.5)
    data = open(dump, "rb").read()
    expect = b"\0\0\0\1" + sps + b"\0\0\0\1" + pps + b"\0\0\0\1" + sps + b"\0\0\0\1" + pps + b"\0\0\0\1" + \
        frames[0] + bytes([0, 0, 0, 1, 9, 0xf0]) + b"\0\0\0\1" + frames[1] + bytes([0, 0, 0, 1, 9, 0xf0])
    check("screen: Annex B in the dump (%d bytes)" % len(data), data == expect)
    wavs = [w for w in os.listdir(audio_dir) if w.endswith(".wav")]
    wav = open(os.path.join(audio_dir, wavs[0]), "rb").read()
    check("audio: WAV of 3520 frames, first samples 1000 / -1000",
          len(wavs) == 1 and len(wav) == 44 + 3520 * 4 and struct.unpack("<hh", wav[44:48]) == (1000, -1000))
    s.close()
    print("ALL OK")


if __name__ == "__main__":
    main()
