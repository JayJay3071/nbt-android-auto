"""Build pinned Mbed TLS and run a real TLS 1.2 memory-transport test on the VM.

No vehicle access or system-library replacement. Requires Python cryptography
(already available in .local/python-packages from the SSH setup). Generated
private keys are disposable laboratory fixtures, saved only in ignored build/.
"""
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import time
import urllib.request
import uuid
from datetime import datetime, timezone
from mbedtls_legacy_time import patch_x509

ROOT = Path(__file__).resolve().parents[1]
VERSION = "3.6.7"
SHA256 = "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6"
URL = f"https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-{VERSION}/mbedtls-{VERSION}.tar.bz2"


def run(args, **kwargs):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


def fixtures():
    local_packages = ROOT / ".local/python-packages"
    if local_packages.is_dir():
        sys.path.insert(0, str(local_packages))
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID

    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Disposable QNX laboratory CA")])

    def builder(subject, public_key):
        return (x509.CertificateBuilder().subject_name(subject).issuer_name(name)
                .public_key(public_key).serial_number(x509.random_serial_number())
                .not_valid_before(datetime(2000, 1, 1, tzinfo=timezone.utc))
                .not_valid_after(datetime(2040, 1, 1, tzinfo=timezone.utc)))

    ca = (builder(name, ca_key.public_key()).add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
          .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, None, None), True)
          .sign(ca_key, hashes.SHA256()))
    files = {"lab-ca.pem": ca.public_bytes(serialization.Encoding.PEM)}
    for role, purpose in [("server", ExtendedKeyUsageOID.SERVER_AUTH), ("client", ExtendedKeyUsageOID.CLIENT_AUTH)]:
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "qnx-aa-lab.local")])
        cert = (builder(subject, key.public_key())
                .add_extension(x509.BasicConstraints(ca=False, path_length=None), True)
                .add_extension(x509.SubjectAlternativeName([x509.DNSName("qnx-aa-lab.local")]), False)
                .add_extension(x509.ExtendedKeyUsage([purpose]), False)
                .add_extension(x509.KeyUsage(True, False, True, False, False, False, False, None, None), True)
                .sign(ca_key, hashes.SHA256()))
        files[f"lab-{role}.pem"] = cert.public_bytes(serialization.Encoding.PEM)
        files[f"lab-{role}-key.pem"] = key.private_bytes(serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    return files


def main():
    opts = ["-F", str(ROOT / ".local/ssh_config"), "-o",
            "UserKnownHostsFile=" + str(ROOT / ".local/known_hosts"),
            "-o", "GlobalKnownHostsFile=none", "-o", "StrictHostKeyChecking=yes",
            "-o", "BatchMode=yes", "-o", "ClearAllForwardings=yes",
            "-o", "ProxyCommand=none", "-o", "ProxyJump=none",
            "-o", "PermitLocalCommand=no", "-o", "ForwardAgent=no"]
    config = run(["ssh", *opts, "-G", "qnx-build"]).decode().splitlines()
    if not all(x in config for x in ["hostname 192.168.56.10", "user nbtbuild", "port 22"]):
        raise SystemExit("This workflow is restricted to the development VM")
    run(["ssh", *opts, "qnx-build", 'test "$(uname -s)" = QNX && test "$(uname -p)" = x86 && '
         'test "$(uname -r)" = 6.5.0 && test "$(id -u)" != 0'], timeout=20)
    cache = ROOT / ".local" / f"mbedtls-{VERSION}.tar.bz2"
    if not cache.exists():
        urllib.request.urlretrieve(URL, cache)
    if hashlib.sha256(cache.read_bytes()).hexdigest() != SHA256:
        raise SystemExit("Release archive SHA256 mismatch")
    run_id = "aa-tls-" + time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + uuid.uuid4().hex[:8]
    out = ROOT / "build" / run_id
    out.mkdir(parents=True)
    files = fixtures()
    files["test_tls_memory.c"] = (ROOT / "src/android_auto/test_tls_memory.c").read_bytes()
    files["test_legacy_time.c"] = (ROOT / "src/android_auto/test_legacy_time.c").read_bytes()
    commands = f'''set -e
export QNX_HOST=/usr/qnx650/host/qnx6/x86
export QNX_TARGET=/usr/qnx650/target/qnx6
export PATH="$QNX_HOST/usr/bin:$PATH"
cd mbedtls-{VERSION}
make lib CC='qcc -V4.4.2,gcc_ntox86' AR=ntox86-ar CFLAGS='-O2 -D_QNX_SOURCE -Wc,-std=gnu99'
cd ..
qcc -V4.4.2,gcc_ntox86 -O2 -Wall -Wextra -D_QNX_SOURCE -Wc,-std=gnu99 -Imbedtls-{VERSION}/include test_tls_memory.c mbedtls-{VERSION}/library/libmbedtls.a mbedtls-{VERSION}/library/libmbedx509.a mbedtls-{VERSION}/library/libmbedcrypto.a -lsocket -o test_tls_memory
./test_tls_memory
qcc -V4.4.2,gcc_ntox86 -O2 -Wall -Wextra -Imbedtls-{VERSION}/include test_legacy_time.c mbedtls-{VERSION}/library/libmbedx509.a mbedtls-{VERSION}/library/libmbedcrypto.a -lsocket -o test_legacy_time
./test_legacy_time
'''
    files["run.sh"] = commands.encode()
    archive = out / "source.tar"
    disabled = ["MBEDTLS_AESNI_C", "MBEDTLS_HAVE_ASM", "MBEDTLS_PADLOCK_C"]
    with tarfile.open(archive, "w", format=tarfile.GNU_FORMAT) as target:
        def add(name, data):
            info = tarfile.TarInfo(name)
            info.mode, info.mtime, info.size = 0o600, 946684800, len(data)
            target.addfile(info, io.BytesIO(data))
        with tarfile.open(cache) as source:
            for item in source:
                path = Path(item.name)
                if path.is_absolute() or ".." in path.parts or path.parts[0] != f"mbedtls-{VERSION}":
                    raise RuntimeError("Unexpected release archive path")
                if not item.isfile():
                    if not item.isdir():
                        raise RuntimeError("Unexpected release archive member")
                    continue
                data = source.extractfile(item).read()
                if item.name == f"mbedtls-{VERSION}/library/x509.c":
                    data = patch_x509(data.decode()).encode()
                    files["qnx-x509-legacy-time.c"] = data
                if item.name.endswith("/include/mbedtls/mbedtls_config.h"):
                    text = data.decode()
                    for option in disabled:
                        text, count = re.subn(r"^#define " + option + r"\s*$",
                                             "/* QNX GCC 4.4: disabled " + option + " */",
                                             text, flags=re.M)
                        if count != 1:
                            raise RuntimeError("Expected exactly one config definition: " + option)
                    data = text.encode()
                    files["qnx-mbedtls-config.h"] = data
                add(item.name, data)
        for name, data in files.items():
            add(name, data)
    (out / "inputs.json").write_text(json.dumps({"url": URL, "archive_sha256": SHA256,
        "disabled": disabled, "fixture_keys": "generated lab-only, never for Android Auto",
        "files_sha256": {n: hashlib.sha256(b).hexdigest() for n, b in files.items()}}, indent=2))
    remote = ".nbt-build/" + run_id
    run(["ssh", *opts, "qnx-build", "umask 077; mkdir " + remote], timeout=20)
    run(["scp", "-O", *opts, str(archive), "qnx-build:" + remote + "/source.tar"], timeout=90)
    print("VM TLS build:", remote, flush=True)
    process = subprocess.run(["ssh", *opts, "qnx-build", "cd " + remote +
                 " && tar xf source.tar && sh run.sh"], stdout=subprocess.PIPE,
                 stderr=subprocess.STDOUT, timeout=300)
    log = process.stdout.decode(errors="replace")
    (out / "build-test.log").write_text(log, encoding="utf-8")
    markers = ["CRYPTO_SELFTESTS_OK", "TLS12_MTLS_OK", "TLS_BINARY_ROUNDTRIP_OK",
               "TLS_TAMPER_REJECTED", "TLS_WRONG_HOST_REJECTED", "QNX_TLS_MEMORY_TEST_OK",
               "LEGACY_TIME_NORMALIZATION_OK"]
    passed = process.returncode == 0 and all(m in log for m in markers)
    summary = {"passed": passed, "ssh_exit": process.returncode, "remote": remote,
               "markers": {m: m in log for m in markers}, "vehicle_tested": False,
               "android_auto_handshake_tested": False}
    if passed:
        artifacts = ["test_tls_memory"] + [f"mbedtls-{VERSION}/library/{n}.a"
                      for n in ["libmbedtls", "libmbedx509", "libmbedcrypto"]]
        summary["artifacts_sha256"] = {}
        for name in artifacts:
            dest = out / Path(name).name
            run(["scp", "-O", *opts, "qnx-build:" + remote + "/" + name, str(dest)], timeout=30)
            data = dest.read_bytes()
            if name == "test_tls_memory" and (data[:6] != b"\x7fELF\x01\x01" or data[18:20] != b"\x03\x00"):
                raise RuntimeError("Unexpected test binary architecture")
            summary["artifacts_sha256"][dest.name] = hashlib.sha256(data).hexdigest()
    (out / "results.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    print("Evidence:", out)
    if not passed:
        print("\n".join(log.splitlines()[-25:]))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
