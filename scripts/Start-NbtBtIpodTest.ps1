# Car test "iPhone over Bluetooth" (2026-11-23), driven from the PC through the open SSH session.
#   powershell -ExecutionPolicy Bypass -File scripts\Start-NbtBtIpodTest.ps1 tap       upload + start the recorder (iPhone BT off first)
#   powershell -ExecutionPolicy Bypass -File scripts\Start-NbtBtIpodTest.ps1 status    short look
#   powershell -ExecutionPolicy Bypass -File scripts\Start-NbtBtIpodTest.ps1 tapstop   stop recorder, fetch logs
#   powershell -ExecutionPolicy Bypass -File scripts\Start-NbtBtIpodTest.ps1 fake      our driver at /fs/btipod (iPhone BT off)
#   powershell -ExecutionPolicy Bypass -File scripts\Start-NbtBtIpodTest.ps1 fakestop  stop it, fetch logs
# Writes only into /fs/ram3/aatool. Logs land in .local\car-logs\btipod-<time>.
#   ... iap / iapwms (own A2DP device, SBC 48 kHz; iapwms44 = 44.1 kHz) / iapstatus / iapstop: OnePlus faked as "iPod over Bluetooth capable" (scripts/car-iap-fake.sh)
param([Parameter(Mandatory = $true)][ValidateSet('tap', 'status', 'tapstop', 'fake', 'fakestop', 'iap', 'iapblock', 'iapurl', 'iapwms', 'iapwms44', 'iapstatus', 'iapstop', 'iapinstall')][string]$Step)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$py = if ($env:NBT_PYTHON) { $env:NBT_PYTHON } else { 'python' }
$req = Join-Path $root '.local\nbt_request.py'
$b = Join-Path $root 'build\aa-entry-20261106'
$T = '/fs/ram3/aatool'
function Put($local, $remote, $mode, $text) {
    $take = if ($text) { "tr -d '\015' > $remote" } else { "cat > $remote" }
    $out = & $py $req "mkdir -p $T && $take && chmod $mode $remote && ls -l $remote" --input-file $local --timeout 120
    if ($LASTEXITCODE -ne 0) { throw "upload of $local failed: $out" }
    Write-Host $out
}
function Fetch($names, $folder = 'btipod-result') {
    $dst = Join-Path $root (".local\car-logs\btipod-" + (Get-Date -Format 'yyyyMMdd-HHmm'))
    New-Item -ItemType Directory -Force $dst | Out-Null
    foreach ($n in $names) {
        # one cat over ~1 MB fails on this link: fetch in 512 KiB pieces and join them here
        $answer = "" + (& $py $req "ls -l $T/$folder/$n | sed 's/  */ /g' | cut -d' ' -f5" --timeout 30)
        if ($answer -notmatch '^\s*(\d+)') { Write-Warning "$n : no size ($answer)"; continue }
        $size = [int64]$Matches[1]
        $out = Join-Path $dst $n
        $parts = @()
        for ($i = 0; $i * 524288 -lt $size; $i++) {
            $p = "$out.part$i"
            & $py $req "dd if=$T/$folder/$n bs=524288 skip=$i count=1 2>/dev/null" --timeout 180 --output-file $p | Out-Null
            $parts += $p
        }
        $fs = [IO.File]::Create($out)
        foreach ($p in $parts) { $bytes = [IO.File]::ReadAllBytes($p); $fs.Write($bytes, 0, $bytes.Length); Remove-Item $p }
        $fs.Close()
        if ((Get-Item $out).Length -ne $size) { Write-Warning "$n : got $((Get-Item $out).Length) of $size bytes" }
    }
    Get-ChildItem $dst | Select-Object Name, Length | Format-Table -AutoSize
}
if ($Step -eq 'tap' -or $Step -eq 'fake') {
    Put "$b\nbt_ipod_tap" "$T/nbt_ipod_tap" 700 $false
    Put "$b\nbt_aa_ipod" "$T/nbt_aa_ipod.bt" 700 $false
    Put "$root\scripts\car-btipod-test.sh" "$T/car-btipod-test.sh" 644 $true
}
if ($Step -eq 'iapinstall') {
    # permanent: active at the next service / NBT start (boot.sh starts iap.sh; old boot.sh kept as boot.sh.pre-iap)
    $P = '/var/opt/hmi/data/rw/NbtAndroidAutoRT'
    Put "$b\nbt_aa_wms" "$P/nbt_aa_wms.new" 700 $false
    Put "$b\nbt_ipod_tap" "$P/nbt_ipod_tap.new" 700 $false
    Put "$b\nbt_aa_ipod" "$P/nbt_aa_ipod.new" 700 $false
    Put "$root\scripts\nbt-aa-iap.sh" "$P/iap.sh.new" 700 $true
    Put "$root\scripts\nbt-aa-boot.sh" "$P/boot.sh.new" 700 $true
    & $py $req "cd $P && sh -n boot.sh.new && sh -n iap.sh.new && cp boot.sh boot.sh.pre-iap && mv nbt_aa_wms.new nbt_aa_wms && mv nbt_ipod_tap.new nbt_ipod_tap && mv nbt_aa_ipod.new nbt_aa_ipod && mv iap.sh.new iap.sh && mv boot.sh.new boot.sh && rm -f iap.off && echo iap-mode > VERSION.mode && ls -l boot.sh iap.sh nbt_aa_wms nbt_ipod_tap nbt_aa_ipod && echo INSTALLED" --timeout 60
    return
}
if ($Step -like 'iap*') {
    if ($Step -eq 'iap' -or $Step -eq 'iapblock' -or $Step -eq 'iapurl' -or $Step -like 'iapwms*') {
        Put "$b\nbt_aa_wms" "$T/nbt_aa_wms" 700 $false
        Put "$b\nbt_aa_wav_http" "$T/nbt_aa_wav_http" 700 $false
        Put "$b\nbt_ipod_tap" "$T/nbt_ipod_tap" 700 $false
        Put "$b\nbt_aa_ipod" "$T/nbt_aa_ipod.bt" 700 $false
        Put "$root\scripts\car-iap-fake.sh" "$T/car-iap-fake.sh" 644 $true
    }
    $sub = @{ iap = 'start'; iapblock = 'start block'; iapurl = 'start url'; iapwms = 'start wms'; iapwms44 = 'start wms 44100'; iapstatus = 'status'; iapstop = 'stop' }[$Step]
    & $py $req "sh $T/car-iap-fake.sh $sub" --timeout 90
    if ($Step -eq 'iapstop') { Fetch @('avrcp-tap.log', 'iap-loop.log', 'btfake.log', 'slog-full.txt', 'stores.txt', 'wav.log', 'wms.log') 'iap-result' }
    return
}
& $py $req "sh $T/car-btipod-test.sh $Step" --timeout 90
if ($Step -eq 'tapstop') { Fetch @('tap.log', 'state.txt', 'markers.txt', 'slog-full.txt') }
if ($Step -eq 'fakestop') { Fetch @('btfake.log', 'fake-markers.txt', 'fake-stores.txt') }


