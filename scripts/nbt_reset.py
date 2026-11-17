#!/usr/bin/env python3
"""Reboot the NBT head unit over ENET with a diagnostic ECU reset (UDS 11 01, hardReset), the same request
Tool32's STEUERGERAETE_RESET sends. Python 3 standard library only.

    python nbt_reset.py                 find the gateway with the HSFZ 6811 broadcast, then reset
    python nbt_reset.py --host IP       skip discovery, use this gateway address
    python nbt_reset.py --type 2        reset type: 1 hardReset (default), 2 keyOffOnReset, 3 softReset

HSFZ framing as in github.com/Xayanix/nbt-evo-ssh-unlocker (nbt_optdebug_flash.py):
    u32 payload length | u16 control (0x0001 diagnostic, 0x0002 ack) | u8 source | u8 target | UDS bytes
"""
import argparse
import socket
import struct
import sys
import time

TESTER = 0xF4       # diagnostic tester address
HU = 0x63           # head unit (NBT)
HSFZ_PORT = 6801
IDENT_PORT = 6811


def enet_source():
    """Local 169.254.x.x address the ENET cable uses (None if the route points elsewhere, e.g. a VPN)."""
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("169.254.1.1", 9))
        ip = probe.getsockname()[0]
        probe.close()
        if ip.startswith("169.254."):
            return ip
    except OSError:
        pass
    return None


def discover(src, timeout=4.0):
    """HSFZ identification broadcast -> gateway IP (or None)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    if src:
        try:
            s.bind((src, 0))
        except OSError:
            pass
    s.settimeout(timeout)
    try:
        s.sendto(struct.pack(">IH", 0, 0x0011), ("169.254.255.255", IDENT_PORT))
        data, addr = s.recvfrom(512)
    except socket.timeout:
        return None
    finally:
        s.close()
    ident = data[6:].decode("latin1", "replace")
    vin = ident.split("BMWVIN", 1)[1][:17] if "BMWVIN" in ident else "?"
    print("gateway %s, VIN %s" % (addr[0], vin))
    return addr[0]


def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("gateway closed the connection")
        buf += chunk
    return buf


def ecu_reset(host, src, kind=1, timeout=5.0):
    # bound to the ENET address, so the connection cannot leave through another adapter (VPN, Wi-Fi)
    print("connecting to %s:%d from %s" % (host, HSFZ_PORT, src or "the default route"))
    sock = socket.create_connection((host, HSFZ_PORT), timeout=timeout, source_address=(src, 0) if src else None)
    try:
        payload = bytes([TESTER, HU, 0x11, kind])
        print("sending 11 %02X" % kind)
        sock.sendall(struct.pack(">IH", len(payload), 0x0001) + payload)
        deadline = time.time() + timeout
        while time.time() < deadline:
            length, ctrl = struct.unpack(">IH", read_exact(sock, 6))
            body = read_exact(sock, length)
            if ctrl == 0x0002:              # the gateway's ack of our request
                continue
            uds = body[2:]
            if uds[:2] == bytes([0x51, kind]):
                print("head unit accepted the reset (51 %02X); it reboots now" % kind)
                return 0
            if len(uds) >= 3 and uds[0] == 0x7F and uds[1] == 0x11:
                if uds[2] == 0x78:          # responsePending: wait longer
                    deadline = time.time() + timeout
                    continue
                print("head unit refused the reset: 7F 11 %02X" % uds[2])
                return 2
            print("unexpected answer: %s" % uds.hex(" "))
        print("no answer within %.0f s" % timeout)
        return 3
    finally:
        sock.close()


def main():
    ap = argparse.ArgumentParser(description="Reboot the NBT head unit (UDS 11 01 over ENET).")
    ap.add_argument("--host", help="gateway IP (default: discover with the 6811 broadcast)")
    ap.add_argument("--src", help="local ENET address to send from (default: found automatically)")
    ap.add_argument("--type", type=int, choices=(1, 2, 3), default=1,
                    help="1 hardReset (default), 2 keyOffOnReset, 3 softReset")
    a = ap.parse_args()
    src = a.src or enet_source()
    host = a.host or discover(src)
    if not host:
        print("no gateway found: check the ENET cable, or pass --host with the gateway IP")
        return 1
    return ecu_reset(host, src, a.type)


if __name__ == "__main__":
    sys.exit(main())
