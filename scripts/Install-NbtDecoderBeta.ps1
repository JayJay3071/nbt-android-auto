# Beta picture path on the car (2026-11-24): valinet's nbt_decoder with our service, through the open SSH session
# (tools\nbt_ssh_session.py + tools\nbt_request.py), like Update-NbtAaPackage.ps1.
#   powershell -ExecutionPolicy Bypass -File scripts\Install-NbtDecoderBeta.ps1 install -Source build\qnx-build-<time>\programs
#   powershell -ExecutionPolicy Bypass -File scripts\Install-NbtDecoderBeta.ps1 remove
# install: native_service, boot.sh and, if it is in -Source, oneplus_native change in the package (the old ones are kept as
#          *.pre-decoder); new are nbt_decoder and, if it is in -Source, nbt_carplay. Nothing running is touched; active
#          after the next start of the head unit.
# remove:  the kept files come back; nbt_decoder, nbt_carplay and their switches are deleted.
# page:    the beta's Android Auto page (restart button, decoder button; no picture modes): plugin library, page XML and
#          texts from -Source <folder made by src\idrive\make_aa_entry_resources.py with libNbtAndroidAuto.so in it>;
#          the old three files are kept as *.pre-decoder. Active after the next start of the head unit.
# page-remove: the kept page files come back.
# Without a PC: touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/decoder.off = the old picture path from the next start.
param([Parameter(Mandatory = $true)][ValidateSet('install', 'remove', 'page', 'page-remove')][string]$Step, [string]$Source = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$py = if ($env:NBT_PYTHON) { $env:NBT_PYTHON } else { 'python' }
$req = Join-Path $root 'tools\nbt_request.py'
$P = '/var/opt/hmi/data/rw/NbtAndroidAutoRT'
if ($Step -eq 'remove') {
    & $py $req "cd $P && test -f native_service.pre-decoder -a -f boot.sh.pre-decoder && mv native_service.pre-decoder native_service && mv boot.sh.pre-decoder boot.sh && { test ! -f oneplus_native.pre-decoder || mv oneplus_native.pre-decoder oneplus_native; } && rm -f nbt_decoder nbt_carplay decoder.off decoder.hw decoder.args carplay.off carplay.args && echo REMOVED" --timeout 60
    if ($LASTEXITCODE -ne 0) { throw 'remove failed (no *.pre-decoder files in the package?)' }
    return
}
$D = '/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto'
$page = 'libNbtAndroidAuto.so', 'NbtAndroidAuto.xml', 'NbtAndroidAuto_ALL_Texts.zip'
if ($Step -eq 'page-remove') {
    & $py $req "cd $D && for f in $page; do test -f `$f.pre-decoder || exit 1; done && for f in $page; do mv `$f.pre-decoder `$f || exit 1; done && ls -l $page && echo PAGE_REMOVED" --timeout 60
    if ($LASTEXITCODE -ne 0) { throw 'page-remove failed (no *.pre-decoder files in the plugin folder?)' }
    return
}
if (-not $Source) { throw "$Step needs -Source <folder>" }
if ($Step -eq 'page') {
    foreach ($name in $page) {
        $local = Join-Path $Source $name
        $size = (Get-Item $local).Length
        $mode = if ($name -like '*.so') { '755' } else { '644' }
        $answer = "" + (& $py $req "cat > $D/$name.new && chmod $mode $D/$name.new && ls -l $D/$name.new | sed 's/  */ /g' | cut -d' ' -f5" --input-file $local --timeout 120)
        if ($LASTEXITCODE -ne 0 -or $answer -notmatch '^\s*(\d+)' -or $Matches[1] -ne "$size") { throw "upload of $name failed: $answer (expected $size bytes)" }
        Write-Host "uploaded $name ($size bytes)"
    }
    # the restart tool for the page's "NBT neu starten" (the car-specific file "gateway" next to it is left alone)
    $reset = Join-Path $Source 'nbt_reset'
    if (Test-Path $reset) {
        $size = (Get-Item $reset).Length
        $answer = "" + (& $py $req "cat > $P/nbt_reset.new && chmod 700 $P/nbt_reset.new && ls -l $P/nbt_reset.new | sed 's/  */ /g' | cut -d' ' -f5" --input-file $reset --timeout 120)
        if ($LASTEXITCODE -ne 0 -or $answer -notmatch '^\s*(\d+)' -or $Matches[1] -ne "$size") { throw "upload of nbt_reset failed: $answer (expected $size bytes)" }
        & $py $req "mv $P/nbt_reset.new $P/nbt_reset && ls -l $P/nbt_reset $P/gateway" --timeout 30
    }
    & $py $req "cd $D && for f in $page; do test -f `$f.pre-decoder || cp `$f `$f.pre-decoder || exit 1; done && for f in $page; do mv `$f.new `$f || exit 1; done && ls -l && echo PAGE_INSTALLED" --timeout 60
    if ($LASTEXITCODE -ne 0) { throw 'page install failed: the plugin folder is unchanged except for *.new files' }
    Write-Host 'Done. Active after the next start of the head unit.'
    return
}
$names = @('nbt_decoder', 'native_service') + @('oneplus_native', 'nbt_carplay' | Where-Object { Test-Path (Join-Path $Source $_) })
foreach ($name in $names) {
    $local = Join-Path $Source $name
    $size = (Get-Item $local).Length
    # the size on the car must be the size here: a cut-off upload must not become the service
    $answer = "" + (& $py $req "cat > $P/$name.new && chmod 700 $P/$name.new && ls -l $P/$name.new | sed 's/  */ /g' | cut -d' ' -f5" --input-file $local --timeout 180)
    if ($LASTEXITCODE -ne 0 -or $answer -notmatch '^\s*(\d+)' -or $Matches[1] -ne "$size") { throw "upload of $name failed: $answer (expected $size bytes)" }
    Write-Host "uploaded $name ($size bytes)"
}
& $py $req "tr -d '\015' > $P/boot.sh.new && chmod 700 $P/boot.sh.new && sh -n $P/boot.sh.new && echo OK boot.sh" --input-file (Join-Path $PSScriptRoot 'nbt-aa-boot.sh') --timeout 30
if ($LASTEXITCODE -ne 0) { throw 'upload of boot.sh failed' }
# the first install keeps the files from before the beta; a later one must not overwrite that copy with a beta file
& $py $req "cd $P && { test -f native_service.pre-decoder || cp native_service native_service.pre-decoder; } && { test -f boot.sh.pre-decoder || cp boot.sh boot.sh.pre-decoder; } && { test ! -f oneplus_native.new || test -f oneplus_native.pre-decoder || cp oneplus_native oneplus_native.pre-decoder; } && for f in $names boot.sh; do mv `$f.new `$f || exit 1; done && rm -f decoder.off && ls -l $names boot.sh *.pre-decoder && echo INSTALLED" --timeout 60
if ($LASTEXITCODE -ne 0) { throw 'install failed: the package is unchanged except for *.new files' }
Write-Host 'Done. Active after the next start of the head unit.'
