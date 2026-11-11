"""Collect the car-tested build outputs into installer/payload (+ manifest.json with SHA-256 per file).
Usage: python installer/make_payload.py [version]
       python installer/make_payload.py [version] --built build/qnx-build-<time> [--pem-dir <folder>]
The payload contains binaries and the head-unit TLS identity; it is not committed (see .gitignore) but bundled into the
release builds. Rebuild libNbtAndroidAuto.so in the QNX VM after any change to src/idrive/aa_entry_plugin.cpp first.
--built takes the programs and plugin files from a scripts/build_all.py run instead of the developer's build folders;
the PEM files (public NBT-AAD head-unit identity) then come from --pem-dir (default: rt/ of an existing payload,
e.g. unpacked from a release ZIP).
"""
import argparse
import hashlib
import json
import shutil
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / 'installer' / 'payload'
RT = '/var/opt/hmi/data/rw/NbtAndroidAutoRT'
PLUGIN = '/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto'
ENTRY = REPO / 'build' / 'aa-entry-20261106'
PROTO = REPO / 'build' / 'native-prototype-20261106' / 'package'
# (local source, payload name, target path on the car, mode)
FILES = [
    (ENTRY / 'native_service', 'rt/native_service', RT + '/native_service', '700'),
    (ENTRY / 'oneplus_native', 'rt/oneplus_native', RT + '/oneplus_native', '700'),
    (ENTRY / 'nbt_knob', 'rt/nbt_knob', RT + '/nbt_knob', '700'),
    (ENTRY / 'nbt_hwvideo', 'rt/nbt_hwvideo', RT + '/nbt_hwvideo', '700'),
    (ENTRY / 'decode_runner', 'rt/decode_runner', RT + '/decode_runner', '700'),
    (ENTRY / 'nbt_cluster', 'rt/nbt_cluster', RT + '/nbt_cluster', '700'),
    (PROTO / 'accessory_window', 'rt/accessory_window', RT + '/accessory_window', '700'),
    (PROTO / 'fifo_http', 'rt/fifo_http', RT + '/fifo_http', '700'),
    (PROTO / 'cert.pem', 'rt/cert.pem', RT + '/cert.pem', '600'),
    (PROTO / 'key.pem', 'rt/key.pem', RT + '/key.pem', '600'),
    (PROTO / 'ca.pem', 'rt/ca.pem', RT + '/ca.pem', '600'),
    (REPO / 'scripts' / 'nbt-aa-boot.sh', 'rt/boot.sh', RT + '/boot.sh', '700'),
    (REPO / 'scripts' / 'nbt-aa-restart.sh', 'rt/restart.sh', RT + '/restart.sh', '700'),
    (ENTRY / 'NbtAndroidAuto' / 'libNbtAndroidAuto.so', 'plugin/libNbtAndroidAuto.so', PLUGIN + '/libNbtAndroidAuto.so', '755'),
    (ENTRY / 'NbtAndroidAuto' / 'NbtAndroidAuto.xml', 'plugin/NbtAndroidAuto.xml', PLUGIN + '/NbtAndroidAuto.xml', '644'),
    (ENTRY / 'NbtAndroidAuto' / 'NbtAndroidAuto_ALL_Texts.zip', 'plugin/NbtAndroidAuto_ALL_Texts.zip',
     PLUGIN + '/NbtAndroidAuto_ALL_Texts.zip', '644'),
    (ENTRY / 'NbtAndroidAuto' / 'NbtAndroidAuto_ALL_Images.zip', 'plugin/NbtAndroidAuto_ALL_Images.zip',
     PLUGIN + '/NbtAndroidAuto_ALL_Images.zip', '644'),
    (ENTRY / 'NbtAndroidAuto' / 'NbtAndroidAuto.manifest', 'plugin/NbtAndroidAuto.manifest',
     PLUGIN + '/NbtAndroidAuto.manifest', '644'),
]


def built_files(built, pem_dir):
    """FILES with the sources replaced by a scripts/build_all.py output folder."""
    programs, resources = built / 'programs', built / 'plugin-resources'
    result = []
    for source, name, target, mode in FILES:
        leaf = Path(name).name
        if leaf.endswith('.pem'):
            source = pem_dir / leaf
        elif name.startswith('plugin/') and leaf != 'libNbtAndroidAuto.so':
            source = resources / leaf
        elif leaf in ('boot.sh', 'restart.sh'):
            pass  # straight from scripts/
        else:
            source = programs / leaf
        result.append((source, name, target, mode))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('version', nargs='?', default=time.strftime('%Y.%m.%d'))
    parser.add_argument('--built', type=Path, help='output folder of scripts/build_all.py')
    parser.add_argument('--pem-dir', type=Path, default=OUT / 'rt', help='folder with cert.pem, key.pem, ca.pem')
    args = parser.parse_args()
    version = args.version
    if args.built:
        files = built_files(args.built.resolve(), args.pem_dir.resolve())
        pems = {}
        for source, name, _, _ in files:   # read the PEMs before OUT is replaced (they may live in it)
            if name.endswith('.pem'):
                pems[name] = source.read_bytes()
    else:
        files, pems = FILES, {}
        plugin_src = REPO / 'src' / 'idrive' / 'aa_entry_plugin.cpp'
        plugin_bin = ENTRY / 'NbtAndroidAuto' / 'libNbtAndroidAuto.so'
        if plugin_bin.stat().st_mtime < plugin_src.stat().st_mtime:
            sys.exit('libNbtAndroidAuto.so is older than aa_entry_plugin.cpp: rebuild it in the QNX VM first.')
    if OUT.exists():
        shutil.rmtree(OUT)
    entries = []
    for source, name, target, mode in files:
        data = pems[name] if name in pems else source.read_bytes()
        if name.endswith('.sh'):
            data = data.replace(b'\r\n', b'\n')     # the car's shell does not accept CR
        destination = OUT / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        entries.append({'source': name, 'target': target, 'mode': mode, 'size': len(data),
                        'sha256': hashlib.sha256(data).hexdigest()})
        print('%-42s %8d' % (name, len(data)))
    (OUT / 'manifest.json').write_text(json.dumps({'version': version, 'files': entries}, indent=2), encoding='utf-8')
    print('payload', version, '->', OUT)


if __name__ == '__main__':
    main()
