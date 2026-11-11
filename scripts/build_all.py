"""Build every program of the installed package from this repository in the QNX 6.5.0 VM.

What it does:
  1. checks the NBT-AAD checkout (must be commit 41975f2, unmodified)
  2. downloads Mbed TLS 3.6.7 once (SHA-256 checked, cached in .local/), applies the legacy-UTCTime patch and the
     QNX configuration
  3. prepares the NBT-AAD core: upstream milestone-7 + scripts/aa_session_overlay.py + patches/*.patch + our
     Mbed TLS crypto backend (src/android_auto/aa_cryptor_mbedtls.cpp)
  4. sends everything plus src/ and scripts/build-qnx.sh to the VM and runs the build there
  5. fetches the programs into build/qnx-build-<time>/ and creates the plugin's text/image files
  6. optionally compares the result with an installer payload (byte by byte, also with symbols stripped)

Requirements: Python 3.8+, git, OpenSSH (ssh/scp) to the VM, the VM user must not be root.
Example:
  git clone https://github.com/Sq00ky/Supra-iDrive6-Android-Auto .local/reference-nbt-aad
  git -C .local/reference-nbt-aad checkout 41975f269b065dbdbc77af9f568387cd22b9ed27
  python scripts/build_all.py --host qnx-build --hmi-libs /home/nbtbuild/hmi-libs --compare installer/payload
"""
import argparse
import hashlib
import io
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
from pathlib import Path

from aa_session_overlay import harden_session
from mbedtls_legacy_time import patch_x509

ROOT = Path(__file__).resolve().parents[1]
NBT_AAD_REVISION = "41975f269b065dbdbc77af9f568387cd22b9ed27"
MBEDTLS_VERSION = "3.6.7"
MBEDTLS_SHA256 = "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6"
MBEDTLS_URL = ("https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-%s/mbedtls-%s.tar.bz2"
               % (MBEDTLS_VERSION, MBEDTLS_VERSION))
# Mbed TLS options the QNX 6.5 gcc 4.4 cannot build
MBEDTLS_DISABLED = ["MBEDTLS_AESNI_C", "MBEDTLS_HAVE_ASM", "MBEDTLS_PADLOCK_C"]
# payload binaries built here (installer/payload/<name>)
PROGRAMS = {"oneplus_native": "rt", "native_service": "rt", "decode_runner": "rt", "fifo_http": "rt",
            "accessory_window": "rt", "nbt_knob": "rt", "nbt_hwvideo": "rt", "nbt_cluster": "rt",
            "libNbtAndroidAuto.so": "plugin"}


def git(*args, cwd=None):
    return subprocess.run(["git", *args], cwd=cwd, check=True, stdout=subprocess.PIPE).stdout.decode().strip()


class Archive:
    """Deterministic tar archive (fixed owner, mode and an old timestamp the VM clock accepts)."""

    def __init__(self, path):
        self.tar = tarfile.open(path, "w", format=tarfile.GNU_FORMAT)

    def add(self, name, data, mode=0o600):
        info = tarfile.TarInfo(name)
        info.size, info.mode, info.mtime = len(data), mode, 946684800
        self.tar.addfile(info, io.BytesIO(data))

    def close(self):
        self.tar.close()


def text(path):
    return path.read_bytes().replace(b"\r\n", b"\n")


def add_mbedtls(archive):
    cache = ROOT / ".local" / ("mbedtls-%s.tar.bz2" % MBEDTLS_VERSION)
    cache.parent.mkdir(exist_ok=True)
    if not cache.exists():
        print("downloading", MBEDTLS_URL)
        urllib.request.urlretrieve(MBEDTLS_URL, cache)
    if hashlib.sha256(cache.read_bytes()).hexdigest() != MBEDTLS_SHA256:
        raise SystemExit("Mbed TLS archive: SHA-256 mismatch (%s)" % cache)
    top = "mbedtls-" + MBEDTLS_VERSION
    with tarfile.open(cache) as source:
        for item in source:
            path = Path(item.name)
            if path.is_absolute() or ".." in path.parts or path.parts[0] != top:
                raise SystemExit("Mbed TLS archive: unexpected path " + item.name)
            if not item.isfile():
                continue
            data = source.extractfile(item).read()
            if item.name == top + "/library/x509.c":
                data = patch_x509(data.decode()).encode()
            if item.name.endswith("/include/mbedtls/mbedtls_config.h"):
                config = data.decode()
                for option in MBEDTLS_DISABLED:
                    config, count = re.subn(r"^#define " + option + r"\s*$", "/* QNX GCC 4.4: disabled " + option + " */",
                                            config, flags=re.M)
                    if count != 1:
                        raise SystemExit("Mbed TLS config: expected exactly one " + option)
                data = config.encode()
            archive.add(item.name, data, 0o700 if item.mode & 0o100 else 0o600)


def add_nbt_aad(archive, checkout):
    if git("-C", str(checkout), "rev-parse", "HEAD") != NBT_AAD_REVISION:
        raise SystemExit("NBT-AAD checkout must be at commit " + NBT_AAD_REVISION)
    if git("-C", str(checkout), "status", "--porcelain"):
        raise SystemExit("NBT-AAD checkout must be unmodified (the patches are applied to a copy)")
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        base = checkout / "milestone-7"
        for folder in ("src", "include"):
            for path in (base / folder).rglob("*"):
                if path.is_file():
                    target = work / "milestone-7" / path.relative_to(base)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(text(path))
        src = work / "milestone-7" / "src"
        # 1. checked edits of the session (stricter protocol phases, TLS error handling)
        session = src / "aa_session.cpp"
        session.write_bytes(harden_session(session.read_bytes().decode()).encode())
        # 2. Mbed TLS crypto backend instead of upstream's OpenSSL one
        (src / "aa_cryptor.cpp").write_bytes(text(ROOT / "src/android_auto/aa_cryptor_mbedtls.cpp"))
        (src / "aa_cryptor.hpp").write_bytes(text(ROOT / "src/android_auto/aa_cryptor.hpp"))
        # 3. patches/*.patch (paths milestone-7/src/...)
        for patch in sorted((ROOT / "patches").glob("*.patch")):
            subprocess.run(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=work, check=True)
            print("applied", patch.name)
        for path in sorted((work / "milestone-7").rglob("*")):
            if path.is_file():
                archive.add("reference/" + path.relative_to(work / "milestone-7").as_posix(), path.read_bytes())


def add_repo_sources(archive):
    for folder in ("src/android_auto", "src/idrive"):
        for path in sorted((ROOT / folder).rglob("*")):
            if path.is_file() and path.suffix in (".c", ".cpp", ".h", ".hpp"):
                archive.add(path.relative_to(ROOT).as_posix(), text(path))
    archive.add("build-qnx.sh", text(ROOT / "scripts/build-qnx.sh"), 0o700)


def add_expected(archive, payload):
    for name, folder in PROGRAMS.items():
        path = payload / folder / name
        if path.is_file():
            archive.add("expected/" + name, path.read_bytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="qnx-build", help="ssh host of the QNX VM (default: qnx-build)")
    parser.add_argument("--ssh-config", type=Path, default=ROOT / ".local/ssh_config")
    parser.add_argument("--known-hosts", type=Path, default=ROOT / ".local/known_hosts")
    parser.add_argument("--identity", type=Path, help="ssh private key for the VM (if not set in the ssh config)")
    parser.add_argument("--nbt-aad", type=Path, default=ROOT / ".local/reference-nbt-aad",
                        help="NBT-AAD checkout at commit 41975f2")
    parser.add_argument("--hmi-libs", default="$HOME/.nbt-build/hmi-libs",
                        help="folder IN THE VM with libAppFramework.A.so and libCoreFramework.A.so from your NBT")
    parser.add_argument("--compare", type=Path, help="installer payload folder to compare with (e.g. installer/payload)")
    args = parser.parse_args()

    ssh = []
    if args.ssh_config.is_file():
        ssh += ["-F", str(args.ssh_config)]
    if args.known_hosts.is_file():
        ssh += ["-o", "UserKnownHostsFile=" + str(args.known_hosts)]
    if args.identity:
        ssh += ["-i", str(args.identity)]
    ssh += ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes"]

    def remote(command, timeout=60):
        return subprocess.run(["ssh", *ssh, args.host, command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout)

    probe = remote('test "`uname -s`" = QNX && test "`uname -r`" = 6.5.0 && test "`id -u`" != 0 && echo VM_OK')
    if b"VM_OK" not in probe.stdout:
        raise SystemExit("The host is not a QNX 6.5.0 VM with a non-root user:\n" + probe.stdout.decode(errors="replace"))

    run_id = "qnx-build-" + time.strftime("%Y%m%dT%H%M%S")
    out = ROOT / "build" / run_id
    out.mkdir(parents=True)
    archive_path = out / "source.tar"
    archive = Archive(archive_path)
    add_mbedtls(archive)
    add_nbt_aad(archive, args.nbt_aad.resolve())
    add_repo_sources(archive)
    if args.compare:
        add_expected(archive, args.compare.resolve())
    archive.close()

    folder = ".nbt-build/" + run_id
    remote("umask 077; mkdir -p " + folder)
    subprocess.run(["scp", "-O", "-q", *ssh, str(archive_path), "%s:%s/source.tar" % (args.host, folder)],
                   check=True, timeout=300)
    print("building in the VM:", folder, flush=True)
    result = remote("cd %s && tar xf source.tar && HMI_LIBS=%s sh build-qnx.sh" % (folder, args.hmi_libs), timeout=1800)
    log = result.stdout.decode(errors="replace")
    (out / "build.log").write_text(log, encoding="utf-8")
    print(log[log.find("== oneplus_native"):] if "== oneplus_native" in log else log[-3000:])
    if result.returncode != 0 or "BUILD_QNX_DONE" not in log:
        raise SystemExit("VM build failed, full log: " + str(out / "build.log"))

    programs = out / "programs"
    programs.mkdir()
    for name in PROGRAMS:
        fetched = subprocess.run(["scp", "-O", "-q", *ssh, "%s:%s/out/%s" % (args.host, folder, name), str(programs / name)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if fetched.returncode != 0:
            print("not built:", name)
    resources = out / "plugin-resources"
    subprocess.run([sys.executable, str(ROOT / "src/idrive/make_aa_entry_resources.py"), str(resources)], check=True)
    shutil.copy(ROOT / "scripts/nbt-aa-boot.sh", programs / "boot.sh")
    shutil.copy(ROOT / "scripts/nbt-aa-restart.sh", programs / "restart.sh")
    print("programs:", programs)
    print("plugin XML/manifest/texts:", resources)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
