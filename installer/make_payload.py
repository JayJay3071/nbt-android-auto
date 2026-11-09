"""Collect the car-tested build outputs into installer/payload (+ manifest.json with SHA-256 per file).
Usage: python installer/make_payload.py [version]
The payload contains binaries and the head-unit TLS identity; it is not committed (see .gitignore) but bundled into the
release builds. Rebuild libNbtAndroidAuto.so in the QNX VM after any change to src/idrive/aa_entry_plugin.cpp first.
"""
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


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else time.strftime('%Y.%m.%d')
    plugin_src = REPO / 'src' / 'idrive' / 'aa_entry_plugin.cpp'
    plugin_bin = ENTRY / 'NbtAndroidAuto' / 'libNbtAndroidAuto.so'
    if plugin_bin.stat().st_mtime < plugin_src.stat().st_mtime:
        sys.exit('libNbtAndroidAuto.so is older than aa_entry_plugin.cpp: rebuild it in the QNX VM first.')
    if OUT.exists():
        shutil.rmtree(OUT)
    entries = []
    for source, name, target, mode in FILES:
        data = source.read_bytes()
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
