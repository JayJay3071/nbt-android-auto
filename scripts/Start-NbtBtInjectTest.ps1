# Car test of the Bluetooth-entry injection (2026-11-23), driven from the PC through the open SSH session.
#   powershell -ExecutionPolicy Bypass -File scripts\Start-NbtBtInjectTest.ps1 start    upload into RAM and start
#   powershell -ExecutionPolicy Bypass -File scripts\Start-NbtBtInjectTest.ps1 stop     stop, fetch the logs
#   powershell -ExecutionPolicy Bypass -File scripts\Start-NbtBtInjectTest.ps1 install  make it permanent (next start)
# "start" writes only into /fs/ram3/aatool (gone after an NBT restart). Phone: connected by Bluetooth + USB, AA plays.
param([Parameter(Mandatory = $true)][ValidateSet('start', 'stop', 'install')][string]$Step)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$py = if ($env:NBT_PYTHON) { $env:NBT_PYTHON } else { 'python' }
$req = Join-Path $root '.local\nbt_request.py'
$b = Join-Path $root 'build\aa-entry-20261106'
$T = '/fs/ram3/aatool'
$P = '/var/opt/hmi/data/rw/NbtAndroidAutoRT'
function Put($local, $remote, $mode, $text) {
    $take = if ($text) { "tr -d '\015' > $remote" } else { "cat > $remote" }
    $out = & $py $req "mkdir -p $T && $take && chmod $mode $remote && ls -l $remote" --input-file $local --timeout 120
    if ($LASTEXITCODE -ne 0) { throw "upload of $local failed: $out" }
    Write-Host $out
}
if ($Step -eq 'start') {
    Put "$b\nbt_bt_inject" "$T/nbt_bt_inject" 700 $false
    Put "$b\nbt_devctl_probe" "$T/probe" 700 $false
    Put "$root\scripts\car-bt-inject-test.sh" "$T/car-bt-inject-test.sh" 644 $true
    Put "$root\scripts\car-bt-sound-watch.sh" "$T/car-bt-sound-watch.sh" 644 $true
    & $py $req "sh $T/car-bt-inject-test.sh start" --timeout 60
}
if ($Step -eq 'stop') {
    & $py $req "sh $T/car-bt-inject-test.sh stop" --timeout 60
    $dst = Join-Path $root (".local\car-logs\bt-inject-" + (Get-Date -Format 'yyyyMMdd-HHmm'))
    New-Item -ItemType Directory -Force $dst | Out-Null
    foreach ($n in @('inject.log', 'btwatch.log', 'btwatch-slog.log', 'slog-bt.log', 'sound.log')) {
        & $py $req "cat $T/inject-result/$n" --timeout 120 --output-file (Join-Path $dst $n) | Out-Null
    }
    Get-ChildItem $dst | Select-Object Name, Length | Format-Table -AutoSize
}
if ($Step -eq 'install') {
    Put "$b\nbt_bt_inject" "$P/nbt_bt_inject.new" 700 $false
    Put "$root\scripts\nbt-aa-bt-inject.sh" "$P/bt-inject.sh.new" 700 $true
    Put "$root\scripts\nbt-aa-boot.sh" "$P/boot.sh.new" 700 $true
    & $py $req "cd $P && sh -n boot.sh.new && sh -n bt-inject.sh.new && cp boot.sh boot.sh.pre-btinject && mv nbt_bt_inject.new nbt_bt_inject && mv bt-inject.sh.new bt-inject.sh && mv boot.sh.new boot.sh && rm -f btinject.off && ls -l boot.sh bt-inject.sh nbt_bt_inject && echo INSTALLED - active after the next service or NBT start" --timeout 60
}
