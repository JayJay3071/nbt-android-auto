# Update the installed Android Auto package on the car (autostart package NbtAndroidAutoRT + HMI plugin library) through
# the open SSH session (tools\nbt_ssh_session.py + tools\nbt_request.py). Each file: upload to RAM, copy as .tmp,
# compare, rename. The running service is not touched; the new files are used from the next iDrive start.
# For end users the installer (installer/) does the same job; this script is the developer shortcut.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\Update-NbtAaPackage.ps1 [-Source <build dir>] [-Python <exe>] [-Res720 on|off]
param([string]$Source = '', [string]$Python = '', [ValidateSet('keep', 'on', 'off')][string]$Res720 = 'keep')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$py = if ($Python) { $Python } elseif ($env:NBT_PYTHON) { $env:NBT_PYTHON } else { 'python' }
$req = Join-Path $root 'tools\nbt_request.py'
$src = if ($Source) { $Source } else { Join-Path $root 'build\aa-entry-20261106' }
$P = '/var/opt/hmi/data/rw/NbtAndroidAutoRT'
$D = '/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto'
$S = '/fs/ram3/nbt-aa-update'
& $py $req "mkdir -p $S && test -d $P && test -d $D && echo TARGETS_OK" --timeout 20
if ($LASTEXITCODE -ne 0) { throw 'package or plugin directory missing' }
# local file -> target path on the car
$files = [ordered]@{
    "$src\native_service"                        = "$P/native_service"
    "$src\oneplus_native"                        = "$P/oneplus_native"
    "$src\nbt_knob"                              = "$P/nbt_knob"
    "$src\nbt_hwvideo"                           = "$P/nbt_hwvideo"
    "$src\decode_runner"                         = "$P/decode_runner"
    "$src\nbt_cluster"                           = "$P/nbt_cluster"
    "$src\NbtAndroidAuto\libNbtAndroidAuto.so"   = "$D/libNbtAndroidAuto.so"
    "$src\NbtAndroidAuto\NbtAndroidAuto.xml"     = "$D/NbtAndroidAuto.xml"
    "$src\NbtAndroidAuto\NbtAndroidAuto_ALL_Texts.zip" = "$D/NbtAndroidAuto_ALL_Texts.zip"
}
foreach ($local in $files.Keys) {
    $target = $files[$local]
    $name = Split-Path $local -Leaf
    $mode = if ($name -like '*.so') { '755' } elseif ($name -like '*.xml' -or $name -like '*.zip') { '644' } else { '700' }
    Write-Host "update $name -> $target"
    & $py $req "cat > $S/$name && cp $S/$name $target.tmp && cmp -s $S/$name $target.tmp && chmod $mode $target.tmp && mv $target.tmp $target && cmp -s $S/$name $target && echo OK $name" --input-file $local --timeout 90
    if ($LASTEXITCODE -ne 0) { throw "update of $name failed" }
}
& $py $req "tr -d '\015' > $S/boot.sh && cp $S/boot.sh $P/boot.sh.tmp && chmod 700 $P/boot.sh.tmp && mv $P/boot.sh.tmp $P/boot.sh && echo OK boot.sh" --input-file (Join-Path $PSScriptRoot 'nbt-aa-boot.sh') --timeout 30
& $py $req "tr -d '\015' > $S/restart.sh && cp $S/restart.sh $P/restart.sh.tmp && chmod 700 $P/restart.sh.tmp && mv $P/restart.sh.tmp $P/restart.sh && echo OK restart.sh" --input-file (Join-Path $PSScriptRoot 'nbt-aa-restart.sh') --timeout 30
if ($Res720 -eq 'on') { & $py $req "touch $P/res720 && echo res720 ON" --timeout 20 }
if ($Res720 -eq 'off') { & $py $req "rm -f $P/res720 && echo res720 OFF" --timeout 20 }
& $py $req "rm -r $S; ls -l $P $D" --timeout 20
Write-Host 'Done. Active after the next iDrive start.'
