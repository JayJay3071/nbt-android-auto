"""lwIP and Monocypher for nbt_carplay (src/carplay), taken by build_all.py into the VM archive.

SPDX-License-Identifier: GPL-3.0-only
Copyright (C) 2026 Valentin Radu

  lwIP 2.2.1 (BSD-3-Clause, https://savannah.nongnu.org/projects/lwip, GitHub mirror lwip-tcpip/lwip, tag
  STABLE-2_2_1_RELEASE): the IPv6 / TCP / UDP stack on the iPhone's NCM link, inside nbt_carplay (the car's own
  network stack is not touched)
  Monocypher 4.0.2 (BSD-2-Clause or CC0-1.0, https://monocypher.org, tag 4.0.2): Ed25519 and X25519 for AirPlay's
  pairing (Mbed TLS has no Ed25519)

Each is cloned once into .local/ (only the tag, without CR LF conversion) and must be at the expected commit; only its
src/ folder goes into the archive (lwip/src/..., monocypher/src/...). Neither is part of this repository.
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEPS = [
    ("lwip", "STABLE-2_2_1_RELEASE", "77dcd25a72509eb83f72b033d219b1d40cd8eb95", "https://github.com/lwip-tcpip/lwip.git"),
    ("monocypher", "4.0.2", "0d85f98c9d9b0227e42cf795cb527dff372b40a4",
     "https://github.com/LoupVaillant/Monocypher.git"),
]


def checkout(name, tag, commit, url):
    path = ROOT / ".local" / ("%s-%s" % (name, tag))
    if not path.exists():
        print("cloning", name, tag, "into", path, flush=True)
        subprocess.run(["git", "-c", "core.autocrlf=false", "-c", "advice.detachedHead=false", "clone", "-q",
                        "--depth", "1", "--branch", tag, url, str(path)], check=True)
    head = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], stdout=subprocess.PIPE,
                          check=True).stdout.decode().strip()
    if head != commit:
        raise SystemExit("%s must be at %s %s (%s)" % (path, name, tag, commit))
    status = subprocess.run(["git", "-C", str(path), "status", "--porcelain"], stdout=subprocess.PIPE,
                            check=True).stdout.decode().strip()
    if status:
        raise SystemExit("%s has local changes" % path)
    return path


def add_carplay_deps(archive):
    for name, tag, commit, url in DEPS:
        path = checkout(name, tag, commit, url)
        for item in sorted((path / "src").rglob("*")):
            if item.is_file() and item.suffix in (".c", ".h"):
                archive.add("%s/%s" % (name, item.relative_to(path).as_posix()), item.read_bytes())
