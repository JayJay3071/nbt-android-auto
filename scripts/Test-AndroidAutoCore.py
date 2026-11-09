"""Build/test a reviewed AA source subset on the QNX VM, never on the vehicle.

Requires the pinned reference checkout (license/notices are preserved in the
build archive), local VM SSH configuration and Python 3. No third-party Python
packages. Results and third-party files stay in ignored build/.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
REVISION = "41975f269b065dbdbc77af9f568387cd22b9ed27"
TESTS = {
    "test_sdr": ["src/sdr_builder.cpp"],
    "test_sdr_drop": ["src/sdr_builder.cpp"],
    "test_audio_format": ["src/sdr_builder.cpp"],
    "test_audio_channels": [],
    "test_audio_acks": ["src/audio/audio_acks.cpp"],
    "test_media_ack": [],
    "test_media_msgs": [],
    "test_session_owner": [],
}
OBJECTS = ["aa_session", "aa_cryptor", "usb_accessory", "usb_direct"]


def run(args, **kwargs):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path,
                        default=ROOT / ".local/reference-nbt-aad")
    args = parser.parse_args()
    reference = args.reference.resolve()
    revision = run(["git", "-C", str(reference), "rev-parse", "HEAD"]).decode().strip()
    if revision != REVISION:
        raise SystemExit("Reference revision does not match reviewed commit")
    if run(["git", "-C", str(reference), "status", "--porcelain"]).strip():
        raise SystemExit("Reference must be clean; apply port changes to the build copy")

    opts = ["-F", str(ROOT / ".local/ssh_config"), "-o",
            "UserKnownHostsFile=" + str(ROOT / ".local/known_hosts"),
            "-o", "GlobalKnownHostsFile=none", "-o", "StrictHostKeyChecking=yes",
            "-o", "BatchMode=yes", "-o", "ClearAllForwardings=yes",
            "-o", "ProxyCommand=none", "-o", "ProxyJump=none",
            "-o", "PermitLocalCommand=no", "-o", "ForwardAgent=no"]
    config = run(["ssh", *opts, "-G", "qnx-build"]).decode().splitlines()
    for expected in ["hostname 192.168.56.10", "user nbtbuild", "port 22"]:
        if expected not in config:
            raise SystemExit("VM restriction failed: " + expected)
    probe = run(["ssh", *opts, "qnx-build",
                 'test "$(uname -s)" = QNX && test "$(uname -p)" = x86 && '
                 'test "$(uname -r)" = 6.5.0 && test "$(id -u)" != 0 && uname -a'],
                timeout=20).decode()
    run_id = "aa-core-" + time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + uuid.uuid4().hex[:8]
    out = ROOT / "build" / run_id
    out.mkdir(parents=True)
    source_root = reference / "milestone-7"
    files = {}

    def collect(relative):
        path = (source_root / relative).resolve()
        relative = path.relative_to(source_root).as_posix()
        if relative in files:
            return
        data = path.read_bytes().replace(b"\r\n", b"\n")
        files[relative] = data
        for header in re.findall(rb'^\s*#\s*include\s*"([^"]+)"', data, re.M):
            header = header.decode()
            candidates = [path.parent / header, source_root / "src" / header,
                          source_root / "include" / header, source_root / "tests" / header]
            found = next((p for p in candidates if p.is_file()), None)
            if found is None:
                raise RuntimeError("Unresolved local include: " + header)
            collect(found.relative_to(source_root))

    for name, sources in TESTS.items():
        for path in ["tests/" + name + ".cpp", *sources]:
            collect(path)
    for name in OBJECTS:
        collect("src/" + name + ".cpp")
    files["LICENSE.md"] = (reference / "LICENSE.md").read_bytes()
    for name in ["usb_inventory", "tls_capabilities", "connection_inventory", "accessory_window", "oneplus_aoa"]:
        files["diagnostics/" + name + ".c"] = (ROOT / "src/android_auto" / (name + ".c")).read_bytes()

    commands = ["set -u", "export QNX_HOST=/usr/qnx650/host/qnx6/x86",
                "export QNX_TARGET=/usr/qnx650/target/qnx6",
                'export PATH="$QNX_HOST/usr/bin:$PATH"', "mkdir bin"]
    cxx = "qcc -V4.4.2,gcc_ntox86_cpp -lang-c++ -O1 -Wall -D_QNX_SOURCE -Wc,-std=gnu++0x -Iinclude -Isrc"
    for name, sources in TESTS.items():
        cmd = cxx + " tests/" + name + ".cpp " + " ".join(sources) + " -lsocket -lm -o bin/" + name
        commands += [f"echo BUILD_{name}", f"if {cmd}; then",
                     f"  ./bin/{name}", "  result=$?",
                     f'  echo RESULT:{name}:$result', "else",
                     f"  echo RESULT:{name}:BUILD_FAILED", "fi"]
    for name in OBJECTS:
        cmd = cxx + " -c src/" + name + ".cpp -o bin/" + name + ".o"
        commands += [cmd, f'echo OBJECT:{name}:$?']
    for name, libs in [("usb_inventory", "-lusbdi"), ("tls_capabilities", ""), ("connection_inventory", ""), ("accessory_window", ""), ("oneplus_aoa", "-lusbdi")]:
        commands += [f"if qcc -V4.4.2,gcc_ntox86 -Wall -Wextra diagnostics/{name}.c {libs} -o bin/{name}; then"]
        if name == "accessory_window":
            commands += ['  if [ "$(id -u)" != 0 ]; then',
                         '    echo PROBE:accessory_window:ROOT_REQUIRED', '  else',
                         '    ./bin/accessory_window', '    echo PROBE:accessory_window:$?', '  fi']
        elif name == "oneplus_aoa":
            commands += ['  ./bin/oneplus_aoa --selftest', '  test $? -eq 0 || exit 1',
                         '  ./bin/oneplus_aoa --switch-aa 0 2', '  test $? -eq 3 || exit 1',
                         '  ./bin/oneplus_aoa --version 0 2', '  test $? -eq 3 || exit 1',
                         '  ./bin/oneplus_aoa --tls-inspect 0 2', '  test $? -eq 3 || exit 1',
                         '  ./bin/oneplus_aoa --query invalid 2', '  test $? -eq 2 || exit 1',
                         '  echo PROBE:oneplus_aoa:GUARDS_OK']
        else:
            commands += [f"  ./bin/{name}", f'  echo PROBE:{name}:$?']
        commands += ["else",
                     f"  echo PROBE:{name}:BUILD_FAILED", "fi"]
    commands += ["echo AA_CORE_RUN_COMPLETE"]
    files["run.sh"] = ("\n".join(commands) + "\n").encode()
    archive = out / "source.tar"
    with tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o600
            info.mtime = 946684800
            tar.addfile(info, io.BytesIO(data))
    (out / "inputs.json").write_text(json.dumps({"revision": revision, "vm": probe,
        "files_sha256": {n: hashlib.sha256(b).hexdigest() for n, b in files.items()}}, indent=2))
    remote = ".nbt-build/" + run_id
    run(["ssh", *opts, "qnx-build", "umask 077; mkdir -p .nbt-build; mkdir " + shlex.quote(remote)], timeout=20)
    run(["scp", "-O", *opts, str(archive), "qnx-build:" + remote + "/source.tar"], timeout=60)
    print("VM build:", remote, flush=True)
    process = subprocess.run(["ssh", *opts, "qnx-build", "cd " + shlex.quote(remote) +
                              " && tar xf source.tar && sh run.sh"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
    log = process.stdout.decode(errors="replace")
    (out / "build-test.log").write_text(log, encoding="utf-8")
    results = dict(re.findall(r"^RESULT:([^:]+):(.+)$", log, re.M))
    objects = dict(re.findall(r"^OBJECT:([^:]+):(.+)$", log, re.M))
    probes = dict(re.findall(r"^PROBE:([^:]+):(.+)$", log, re.M))
    summary = {"reference": revision, "remote": remote, "ssh_exit": process.returncode,
               "complete": "AA_CORE_RUN_COMPLETE" in log, "tests": results,
               "compile_only": objects, "diagnostics": probes,
               "vehicle_tested": False, "tls_handshake_tested": False}
    (out / "results.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2), flush=True)
    print("Evidence:", out, flush=True)
    # Fetch only our diagnostic binaries, never the reference daemon.
    for name in ["usb_inventory", "tls_capabilities", "connection_inventory", "accessory_window", "oneplus_aoa"]:
        if name not in probes or probes[name] == "BUILD_FAILED":
            continue
        run(["scp", "-O", *opts, "qnx-build:" + remote + "/bin/" + name, str(out / name)], timeout=30)
        data = (out / name).read_bytes()
        if data[:6] != b"\x7fELF\x01\x01" or data[18:20] != b"\x03\x00":
            raise RuntimeError("Unexpected diagnostic architecture")
    passed = (process.returncode == 0 and summary["complete"] and
              len(results) == len(TESTS) and all(v == "0" for v in results.values()) and
              probes.get("connection_inventory") == "0" and
              probes.get("accessory_window") in ("0", "ROOT_REQUIRED") and
              probes.get("oneplus_aoa") == "GUARDS_OK")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
