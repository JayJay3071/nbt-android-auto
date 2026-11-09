# Upload the 2026-11-06 night build to the car's RAM test directory through the user's open SSH session
# (tools\nbt_request.py). Writes ONLY /fs/ram3/nbt-video-test/ (RAM, gone after a restart). Starts nothing.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\Upload-NbtAaTest.ps1
param([string]$Python = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$py = if ($Python) { $Python } elseif ($env:NBT_PYTHON) { $env:NBT_PYTHON } else { 'python' }
$req = Join-Path $root 'tools\nbt_request.py'
$src = Join-Path $root 'build\aa-entry-20261106'
$T = '/fs/ram3/nbt-video-test'
& $py $req "mkdir -p $T; chmod 700 $T; ls -l $T" --timeout 20
if ($LASTEXITCODE -ne 0) { throw 'mkdir failed' }
# local name -> name on the car (native_service is started by car-wal-start.sh as native_service_live)
$files = [ordered]@{ 'native_service' = 'native_service_live'; 'oneplus_native' = 'oneplus_native';
                     'nbt_knob' = 'nbt_knob'; 'nbt_hwvideo' = 'nbt_hwvideo'; 'decode_runner' = 'decode_runner' }
foreach ($k in $files.Keys) {
    $local = Join-Path $src $k
    $remote = "$T/$($files[$k])"
    $size = (Get-Item $local).Length
    Write-Host "upload $k -> $remote ($size bytes)"
    & $py $req "cat > $remote.tmp && chmod 700 $remote.tmp && mv $remote.tmp $remote && ls -l $remote" --input-file $local --timeout 60
    if ($LASTEXITCODE -ne 0) { throw "upload of $k failed" }
}
# test helpers: clip runner, beep files for the audio probe, NmeCmdLine copy (RAM)
& $py $req "cat > $T/video_runner && chmod 700 $T/video_runner" --input-file (Join-Path $root 'build\native-video-test-20261106\video_runner') --timeout 60 | Out-Null
foreach ($k in 1..6) { & $py $req "cat > $T/beep$k.wav" --input-file (Join-Path $root "build\audio-test-20261107\beep$k.wav") --timeout 60 | Out-Null }
& $py $req "cp /opt/mm/bin/NmeCmdLine /opt/mm/lib/cinemo/cinemo_classes.xml /opt/mm/lib/cinemo/cinemo_links.xml $T/ && chmod 700 $T/NmeCmdLine" --timeout 30 | Out-Null
# size check on the car (the car has no sha256 tool; sizes plus the SHA256 list in docs/STATUS-2026-11-06.md)
& $py $req "ls -l $T" --timeout 20
Write-Host 'Expected sizes:'
foreach ($k in $files.Keys) { Write-Host ("  {0,-22} {1}" -f $files[$k], (Get-Item (Join-Path $src $k)).Length) }
