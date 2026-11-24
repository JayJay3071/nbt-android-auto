# Put the complete developer state of the Android Auto package back on the car, through the open SSH session
# (.local\nbt_ssh_session.py + .local\nbt_request.py): service programs, certificates, start scripts, key map, the
# iPod source tools and the HMI plugin. Written 2026-11-16, after all files under /var/opt/hmi/data/rw had vanished
# from the car (only the empty folders were left).
# Each file: upload to RAM, copy as .tmp, compare, set mode, rename; the plugin manifest goes last, so the HMI never
# sees half a plugin. Nothing is started here.
# Since 2026-11-23 the Android Auto sound goes to the Bluetooth audio entry (sound.args "input_stereo_0 2" + sound.nostop);
# the old iPod-in-the-snap-in tools are only written with -WithIpod.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\Restore-NbtAaInstall.ps1 [-WithIpod]
param([switch]$WithIpod)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$py = if ($env:NBT_PYTHON) { $env:NBT_PYTHON } else { 'python' }
$req = Join-Path $root '.local\nbt_request.py'
$e = Join-Path $root 'build\aa-entry-20261106'
$proto = Join-Path $root 'build\native-prototype-20261106\package'
$P = '/var/opt/hmi/data/rw/NbtAndroidAutoRT'
$D = '/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto'
$S = '/fs/ram3/nbt-aa-update'
# local file, target on the car, mode, 'text' = shell script or text file (carriage returns removed)
$files = @(
    @("$e\native_service", "$P/native_service", '700', ''),
    @("$e\oneplus_native", "$P/oneplus_native", '700', ''),
    @("$e\nbt_knob", "$P/nbt_knob", '700', ''),
    @("$e\nbt_hwvideo", "$P/nbt_hwvideo", '700', ''),
    @("$e\decode_runner", "$P/decode_runner", '700', ''),
    @("$e\fifo_http", "$P/fifo_http", '700', ''),
    @("$e\nbt_cluster", "$P/nbt_cluster", '700', ''),
    @("$e\nbt_a2i_play", "$P/nbt_a2i_play", '700', ''),
    @("$e\accessory_window.ipodtrial", "$P/accessory_window", '700', ''),
    @("$proto\cert.pem", "$P/cert.pem", '600', ''),
    @("$proto\key.pem", "$P/key.pem", '600', ''),
    @("$proto\ca.pem", "$P/ca.pem", '600', ''),
    @("$root\config\mfl.map", "$P/mfl.map", '644', 'text'),
    @("$root\scripts\nbt-aa-boot.sh", "$P/boot.sh", '700', 'text'),
    @("$root\scripts\nbt-aa-restart.sh", "$P/restart.sh", '700', 'text'),
    @("$e\nbt_bt_inject", "$P/nbt_bt_inject", '700', ''),
    @("$root\scripts\nbt-aa-bt-inject.sh", "$P/bt-inject.sh", '700', 'text'),
    @("$e\NbtAndroidAuto\libNbtAndroidAuto.so", "$D/libNbtAndroidAuto.so", '755', ''),
    @("$e\NbtAndroidAuto\NbtAndroidAuto.xml", "$D/NbtAndroidAuto.xml", '644', ''),
    @("$e\NbtAndroidAuto\NbtAndroidAuto_ALL_Texts.zip", "$D/NbtAndroidAuto_ALL_Texts.zip", '644', ''),
    @("$e\NbtAndroidAuto\NbtAndroidAuto_ALL_Images.zip", "$D/NbtAndroidAuto_ALL_Images.zip", '644', '')
)
if ($WithIpod) {
    $files += @(
        @("$e\nbt_aa_ipod", "$P/ipod/nbt_aa_ipod", '700', ''),
        @("$e\nbt_driver_stub", "$P/ipod/stub", '755', ''),
        @("$e\nbt_usb_reset", "$P/ipod/nbt_usb_reset", '700', ''),
        @("$root\experiments\ipod-usb-trials\nbt-aa-ipod-start.sh", "$P/ipod/ipod-start.sh", '644', 'text'),
        @("$root\experiments\ipod-usb-trials\nbt-aa-ipod-takeover.sh", "$P/ipod/ipod-takeover.sh", '644', 'text')
    )
}
# the manifest last
$files += , @("$e\NbtAndroidAuto\NbtAndroidAuto.manifest", "$D/NbtAndroidAuto.manifest", '644', '')
foreach ($f in $files) { if (-not (Test-Path $f[0])) { throw "missing locally: $($f[0])" } }
& $py $req "mkdir -p $S $P $P/ipod $D && chmod 700 $P && echo TARGETS_OK" --timeout 30
if ($LASTEXITCODE -ne 0) { throw 'could not prepare the folders on the car' }
$n = 0
foreach ($f in $files) {
    $name = Split-Path $f[1] -Leaf
    $take = if ($f[3] -eq 'text') { "tr -d '\015' > $S/$name" } else { "cat > $S/$name" }
    $out = & $py $req "$take && cp $S/$name $($f[1]).tmp && cmp -s $S/$name $($f[1]).tmp && chmod $($f[2]) $($f[1]).tmp && mv $($f[1]).tmp $($f[1]) && cmp -s $S/$name $($f[1]) && echo OK $name" --input-file $f[0] --timeout 180
    if ($LASTEXITCODE -ne 0 -or -not ("$out" -match "OK $([regex]::Escape($name))")) { throw "writing $name failed: $out" }
    $n++
    Write-Host ("ok {0,-32} {1,8}" -f $name, (Get-Item $f[0]).Length)
}
& $py $req "test -f $P/vid.dpi || echo 100 > $P/vid.dpi; rm -f $P/disabled; echo 'input_stereo_0 2' > $P/sound.args; touch $P/sound.nostop; echo dev-20261123-bt > $P/VERSION; rm -r $S; ls -l $P $P/ipod $D" --timeout 30
Write-Host "Done: $n files. Start the service with: sh $P/boot.sh  (the menu entry appears after the next iDrive start)"
