# Build the one-file Windows installer: installer\dist\NBT-AndroidAuto-Installer.exe
# Needs: python installer\make_payload.py run first (installer\payload\manifest.json), a venv with pyinstaller + paramiko:
#   python -m venv build\installer-venv; build\installer-venv\Scripts\python -m pip install pyinstaller "paramiko>=3.4,<4"
param([string]$Python = "$PSScriptRoot\..\build\installer-venv\Scripts\python.exe")
$ErrorActionPreference = 'Stop'
if (-not (Test-Path "$PSScriptRoot\payload\manifest.json")) { throw 'payload missing: run python installer\make_payload.py' }
Push-Location $PSScriptRoot
try {
    & $Python -m PyInstaller --noconfirm --clean --onefile --windowed --name NBT-AndroidAuto-Installer `
        --add-data "$PSScriptRoot\payload;payload" --distpath dist --workpath build --specpath build "$PSScriptRoot\nbt_aa_installer.py"
    if ($LASTEXITCODE -ne 0) { throw 'PyInstaller failed' }
    Get-Item dist\NBT-AndroidAuto-Installer.exe | Select-Object Name, Length
} finally { Pop-Location }
