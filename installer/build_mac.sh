#!/bin/sh
# Build the macOS app on a Mac: installer/dist/NBT-AndroidAuto-Installer.app (unsigned: first start with right click ->
# Open). Needs python3 with tkinter (python.org installer) and the payload from make_payload.py (copy installer/payload).
# Without building, the installer also runs directly:  pip3 install "paramiko<4" && python3 nbt_aa_installer.py
set -e
cd "$(dirname "$0")"
test -f payload/manifest.json || { echo 'payload missing: run make_payload.py on the build machine'; exit 1; }
python3 -m venv ../build/installer-venv-mac
../build/installer-venv-mac/bin/python -m pip install -q pyinstaller "paramiko>=3.4,<4"
../build/installer-venv-mac/bin/python -m PyInstaller --noconfirm --clean --windowed --name NBT-AndroidAuto-Installer \
  --add-data "$PWD/payload:payload" --distpath dist --workpath build --specpath build "$PWD/nbt_aa_installer.py"
ls -ld dist/NBT-AndroidAuto-Installer.app
