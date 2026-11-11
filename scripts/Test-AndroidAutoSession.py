"""Link the AA session core to the Mbed TLS backend and test it in the VM.

Requires successful Test-AndroidAutoCore.py and Test-QnxTls.py artifact paths.
No daemon, USB driver, display service or vehicle endpoint is executed.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import time
import uuid
from aa_session_overlay import harden_session

ROOT = Path(__file__).resolve().parents[1]
REVISION = "41975f269b065dbdbc77af9f568387cd22b9ed27"


def run(args, **kwargs):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


def apply_patches(files):
    """Apply patches/*.patch (paths milestone-7/src/...) to the reference/src files, as build_all.py does."""
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for name, data in files.items():
            if name.startswith("reference/src/"):
                target = work / "milestone-7" / name[len("reference/"):]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
        for patch in sorted((ROOT / "patches").glob("*.patch")):
            subprocess.run(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=work, check=True)
        for path in (work / "milestone-7" / "src").rglob("*"):
            if path.is_file():
                files["reference/" + path.relative_to(work / "milestone-7").as_posix()] = path.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-build", type=Path, required=True)
    parser.add_argument("--tls-build", type=Path, required=True)
    args = parser.parse_args()
    core = args.core_build.resolve()
    tls = args.tls_build.resolve()
    for path in [core, tls]:
        path.relative_to(ROOT / "build")
    core_inputs = json.loads((core / "inputs.json").read_text())
    tls_inputs = json.loads((tls / "inputs.json").read_text())
    tls_results = json.loads((tls / "results.json").read_text())
    if core_inputs["revision"] != REVISION or not tls_results["passed"]:
        raise SystemExit("Unexpected reference or failed TLS foundation test")
    files = {}
    with tarfile.open(core / "source.tar") as archive:
        for item in archive:
            if not item.isfile():
                continue
            data = archive.extractfile(item).read()
            if hashlib.sha256(data).hexdigest() != core_inputs["files_sha256"][item.name]:
                raise RuntimeError("Core input hash mismatch")
            if item.name.startswith(("src/", "include/")) or item.name == "LICENSE.md":
                files["reference/" + item.name] = data
    with tarfile.open(tls / "source.tar") as archive:
        for item in archive:
            if not item.isfile():
                continue
            if item.name.startswith("mbedtls-3.6.7/include/"):
                files[item.name] = archive.extractfile(item).read()
            elif item.name.startswith("lab-") and item.name.endswith(".pem"):
                data = archive.extractfile(item).read()
                if hashlib.sha256(data).hexdigest() != tls_inputs["files_sha256"][item.name]:
                    raise RuntimeError("Lab fixture hash mismatch")
                files[item.name] = data
    # The build's exact config must match the config used by the static libraries.
    config_path = "mbedtls-3.6.7/include/mbedtls/mbedtls_config.h"
    if hashlib.sha256(files[config_path]).hexdigest() != tls_inputs["files_sha256"]["qnx-mbedtls-config.h"]:
        raise RuntimeError("Mbed TLS header/config mismatch")
    for name, digest in tls_results["artifacts_sha256"].items():
        if name.endswith(".a"):
            data = (tls / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != digest:
                raise RuntimeError("TLS library hash mismatch")
            files["lib/" + name] = data

    reference = ROOT / ".local/reference-nbt-aad"
    if run(["git", "-C", str(reference), "rev-parse", "HEAD"]).decode().strip() != REVISION:
        raise RuntimeError("Reference revision mismatch")
    if run(["git", "-C", str(reference), "status", "--porcelain"]).strip():
        raise RuntimeError("Reference must remain clean")
    for name in ["src/crash_handler.cpp", "tests/test_session_lifecycle.cpp"]:
        files["reference/" + name] = (reference / "milestone-7" / name).read_bytes().replace(b"\r\n", b"\n")
    for path in sorted((reference / "milestone-7" / "third_party" / "speexdsp").glob("*")):
        files["reference/third_party/speexdsp/" + path.name] = path.read_bytes().replace(b"\r\n", b"\n")
    lifecycle_path = "reference/tests/test_session_lifecycle.cpp"
    lifecycle = files[lifecycle_path].decode()
    start = lifecycle.index("static int countFds() {")
    end = lifecycle.index("\nstatic bool readVersionRequest", start)
    lifecycle = lifecycle[:start] + '''static int countFds() {
    struct rlimit limit;
    assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    assert(limit.rlim_cur <= 65536);
    int n = 0;
    for (int fd = 0; fd < (int)limit.rlim_cur; ++fd) {
        if (fcntl(fd, F_GETFD) != -1) ++n;
        else assert(errno == EBADF);
    }
    return n;
}
''' + lifecycle[end:]
    lifecycle = ("#include <sys/resource.h>\n#include <fcntl.h>\n#include <errno.h>\n" + lifecycle)
    lifecycle = lifecycle.replace("int main() {", "int main() {\n    alarm(90);")
    lifecycle = lifecycle.replace("assert(fds_after <= fds_before + 1);", "assert(fds_after == fds_before);")
    lifecycle = lifecycle.replace("static volatile int g_ended = 0;", '''static int g_ended = 0;
static pthread_mutex_t ended_mutex = PTHREAD_MUTEX_INITIALIZER;
static int endedAdd(int value) {
    pthread_mutex_lock(&ended_mutex);
    int previous = g_ended;
    g_ended += value;
    pthread_mutex_unlock(&ended_mutex);
    return previous;
}''')
    lifecycle = re.sub(r"__sync_fetch_and_add\(&g_ended, ([01])\)", r"endedAdd(\1)", lifecycle)
    files[lifecycle_path] = lifecycle.encode()
    files["reference/src/aa_cryptor.hpp"] = (ROOT / "src/android_auto/aa_cryptor.hpp").read_bytes()
    files["reference/src/aa_cryptor.cpp"] = (ROOT / "src/android_auto/aa_cryptor_mbedtls.cpp").read_bytes()
    session_path = "reference/src/aa_session.cpp"
    files[session_path] = harden_session(files[session_path].decode().replace("\r\n", "\n")).encode()
    apply_patches(files)
    files["test_tls_memory.c"] = (ROOT / "src/android_auto/test_tls_memory.c").read_bytes()
    files["test_session_protocol.cpp"] = (ROOT / "src/android_auto/test_session_protocol.cpp").read_bytes()
    for name in ['tls_usb_probe.h', 'tls_usb_probe.cpp', 'test_tls_usb_probe.cpp', 'oneplus_aoa.c',
                 'aa_video_capture.h', 'aa_video_capture.cpp', 'aa_audio.hpp', 'aa_audio.cpp',
                 'test_video_capture.cpp']:
        files[name] = (ROOT / 'src/android_auto' / name).read_bytes()
    cxx = "qcc -V4.4.2,gcc_ntox86_cpp -lang-c++ -O1 -Wall -D_QNX_SOURCE -Wc,-std=gnu++0x -Imbedtls-3.6.7/include -Ireference/src -Ireference/include"
    libs = "lib/libmbedtls.a lib/libmbedx509.a lib/libmbedcrypto.a -lsocket -lm"
    commands = ["set -e", "export QNX_HOST=/usr/qnx650/host/qnx6/x86",
                "export QNX_TARGET=/usr/qnx650/target/qnx6", 'export PATH="$QNX_HOST/usr/bin:$PATH"',
                cxx + " -DNBT_TLS_BACKEND_TEST test_tls_memory.c reference/src/aa_cryptor.cpp " + libs + " -o test_backend",
                "./test_backend", "mkdir objects"]
    components = ["aa_session", "aa_cryptor", "audio/audio_acks", "sdr_builder", "crash_handler"]
    # browser audio mode, as in build-qnx.sh (objects named audio_<name>.o there and here)
    audio = ["browser_audio", "asound_device", "audio_route", "mixer", "resampler", "drift", "config", "stream_rate",
             "pcm_ring", "playout"]
    for component in components:
        commands.append(cxx + " -c reference/src/" + component + ".cpp -o objects/" + Path(component).name + ".o")
    for name in audio:
        commands.append(cxx + " -c reference/src/audio/" + name + ".cpp -o objects/audio_" + name + ".o")
    commands.append("qcc -V4.4.2,gcc_ntox86 -O1 -Ireference/src -c reference/src/audio/speex.c -o objects/audio_speex.o")
    objects = ["objects/" + Path(n).name + ".o" for n in components] + ["objects/audio_" + n + ".o" for n in audio + ["speex"]]
    commands += ["ntox86-ar rcs libnbt_aa_core.a " + " ".join(objects),
                 cxx + " reference/tests/test_session_lifecycle.cpp libnbt_aa_core.a " + libs + " -o test_session",
                 "./test_session",
                 cxx + " test_session_protocol.cpp libnbt_aa_core.a " + libs + " -o test_protocol",
                 "./test_protocol",
                 cxx + " test_tls_usb_probe.cpp tls_usb_probe.cpp reference/src/aa_cryptor.cpp " + libs + " -o test_tls_usb_probe",
                 "./test_tls_usb_probe",
                 cxx + " test_video_capture.cpp aa_video_capture.cpp aa_audio.cpp libnbt_aa_core.a " + libs + " -o test_video_capture",
                 "./test_video_capture",
                 "qcc -V4.4.2,gcc_ntox86 -Wall -Wextra -DNBT_AA_TLS_HANDSHAKE -c oneplus_aoa.c -o oneplus_aoa.o",
                 cxx + " tls_usb_probe.cpp reference/src/aa_cryptor.cpp oneplus_aoa.o " + libs + " -lusbdi -o oneplus_tls",
                 "./oneplus_tls --selftest",
                 "if ./oneplus_tls --tls-handshake 0 2; then exit 1; else test $? -eq 3; fi",
                 "if ./oneplus_tls --discover 0 2; then exit 1; else test $? -eq 3; fi",
                 "qcc -V4.4.2,gcc_ntox86 -Wall -Wextra -DNBT_AA_TLS_HANDSHAKE -DNBT_AA_CAPTURE -c oneplus_aoa.c -o oneplus_capture.o",
                 cxx + " aa_video_capture.cpp aa_audio.cpp tls_usb_probe.cpp oneplus_capture.o libnbt_aa_core.a " + libs + " -lusbdi -o oneplus_capture",
                 "./oneplus_capture --selftest",
                 "if ./oneplus_capture --capture-video 0 2; then exit 1; else test $? -eq 3; fi",
                 "echo AA_SESSION_VM_TEST_OK"]
    files["run.sh"] = ("\n".join(commands) + "\n").encode()
    run_id = "aa-session-" + time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + uuid.uuid4().hex[:8]
    out = ROOT / "build" / run_id
    out.mkdir(parents=True)
    archive_path = out / "source.tar"
    with tarfile.open(archive_path, "w", format=tarfile.GNU_FORMAT) as archive:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name)
            info.size, info.mode = len(data), 0o600
            info.mtime = 946684800  # Stable timestamp before the VM's intentionally old clock.
            archive.addfile(info, io.BytesIO(data))
    (out / "inputs.json").write_text(json.dumps({"reference": REVISION, "tls_build": str(tls),
        "core_build": str(core), "files_sha256": {n: hashlib.sha256(b).hexdigest() for n, b in files.items()}}, indent=2))
    opts = ["-F", str(ROOT / ".local/ssh_config"), "-o", "UserKnownHostsFile=" + str(ROOT / ".local/known_hosts"),
            "-o", "GlobalKnownHostsFile=none", "-o", "StrictHostKeyChecking=yes", "-o", "BatchMode=yes",
            "-o", "ClearAllForwardings=yes", "-o", "ProxyCommand=none", "-o", "ProxyJump=none",
            "-o", "PermitLocalCommand=no", "-o", "ForwardAgent=no"]
    config = run(["ssh", *opts, "-G", "qnx-build"]).decode().splitlines()
    if not all(x in config for x in ["hostname 192.168.56.10", "user nbtbuild", "port 22"]):
        raise SystemExit("VM restriction failed")
    run(["ssh", *opts, "qnx-build", 'test "$(uname -s)" = QNX && test "$(uname -p)" = x86 && '
         'test "$(uname -r)" = 6.5.0 && test "$(id -u)" != 0'], timeout=20)
    remote = ".nbt-build/" + run_id
    run(["ssh", *opts, "qnx-build", "umask 077; mkdir " + remote], timeout=20)
    run(["scp", "-O", *opts, str(archive_path), "qnx-build:" + remote + "/source.tar"], timeout=60)
    print("VM session build:", remote, flush=True)
    result = subprocess.run(["ssh", *opts, "qnx-build", "cd " + remote + " && tar xf source.tar && sh run.sh"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    log = result.stdout.decode(errors="replace")
    (out / "build-test.log").write_text(log, encoding="utf-8")
    passed = (result.returncode == 0 and "AA_MBEDTLS_BACKEND_OK" in log and
              "AA_TLS_PIN_OK" in log and "AA_TLS_USB_ADAPTER_OK" in log and
              "AA_PROTOCOL_SIMULATOR_OK" in log and "AA_VIDEO_CAPTURE_SIMULATOR_OK" in log and "AA_SESSION_VM_TEST_OK" in log)
    summary = {"passed": passed, "ssh_exit": result.returncode, "remote": remote,
               "backend_passed": "AA_MBEDTLS_BACKEND_OK" in log,
               "lifecycle": re.findall(r"^cycles=.*$", log, re.M),
               "protocol": re.findall(r"^AA_PROTOCOL_.*OK.*$", log, re.M),
               "vehicle_tested": False, "real_phone_handshake_tested": False}
    if passed:
        summary["artifacts_sha256"] = {}
        for name in ["test_backend", "test_session", "test_protocol", "test_tls_usb_probe", "test_video_capture", "oneplus_capture", "oneplus_tls", "libnbt_aa_core.a"]:
            run(["scp", "-O", *opts, "qnx-build:" + remote + "/" + name, str(out / name)], timeout=30)
            data = (out / name).read_bytes()
            if not name.endswith(".a") and (data[:6] != b"\x7fELF\x01\x01" or data[18:20] != b"\x03\x00"):
                raise RuntimeError("Unexpected test binary architecture")
            summary["artifacts_sha256"][name] = hashlib.sha256(data).hexdigest()
    (out / "results.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    print("Evidence:", out)
    if not passed:
        print("\n".join(log.splitlines()[-30:]))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
