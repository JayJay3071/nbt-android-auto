[CmdletBinding()]
param([ValidateSet('Probe','Build')][string]$Action = 'Probe')
$ErrorActionPreference = 'Stop'
function Get-Sha256Hex([string]$Path) {
    # Use .NET so the launcher also works when Get-FileHash is not available.
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
$root = Split-Path $PSScriptRoot -Parent
$config = Join-Path $root '.local/ssh_config'
$known = Join-Path $root '.local/known_hosts'
if (!(Test-Path -LiteralPath $config) -or !(Test-Path -LiteralPath $known)) {
    throw 'First configure .local/ssh_config and verify the VM host key in .local/known_hosts. See README.md.'
}
$options = @('-F', $config, '-o', "UserKnownHostsFile=$known", '-o', 'GlobalKnownHostsFile=none',
    '-o', 'StrictHostKeyChecking=yes', '-o', 'BatchMode=yes', '-o', 'PreferredAuthentications=publickey',
    '-o', 'ClearAllForwardings=yes', '-o', 'ForwardAgent=no', '-o', 'ForwardX11=no',
    '-o', 'PermitLocalCommand=no', '-o', 'ProxyCommand=none', '-o', 'ProxyJump=none')
$effective = & ssh @options -G qnx-build
if ($LASTEXITCODE -ne 0) { throw 'Invalid SSH configuration.' }
if ($effective -notcontains 'hostname 192.168.56.10' -or $effective -notcontains 'port 22') {
    throw 'This workflow is restricted to the QNX VM at 192.168.56.10:22.'
}
if ($effective -contains 'user REPLACE_ME') { throw 'Set the VM username first.' }
$probe = @'
set -eu
test "$(uname -s)" = QNX
test "$(uname -r)" = 6.5.0
test "$(uname -p)" = x86
test "$(id -u)" != 0
uname -a
# SSH does not load the interactive QNX profile. Select the observed SDK explicitly.
QNX_HOST=/usr/qnx650/host/qnx6/x86
QNX_TARGET=/usr/qnx650/target/qnx6
PATH="$QNX_HOST/usr/bin:$PATH"
export QNX_HOST QNX_TARGET PATH
test -d "$QNX_TARGET"
command -v qcc
qcc -V
'@
if ($Action -eq 'Probe') {
    $probe.Replace("`r", '') | & ssh @options qnx-build "tr -d '\015' | sh -s"
    if ($LASTEXITCODE -ne 0) { throw 'VM/toolchain probe failed.' }
    return
}
$run = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$remoteDir = '.nbt-build/' + $run
$source = [IO.File]::ReadAllText((Join-Path $root 'src/hello_nbt.c')).Replace("`r", '')
$delimiter = 'NBT_SOURCE_' + [Guid]::NewGuid().ToString('N')
$script = $probe + "`n" + @"
umask 077
cd "`$HOME"
mkdir -p .nbt-build
mkdir '$remoteDir'
cd '$remoteDir'
cat > hello_nbt.c <<'$delimiter'
$source
$delimiter
qcc -V4.4.2,gcc_ntox86 -O2 -Wall -Wextra hello_nbt.c -o hello_nbt
./hello_nbt
printf 'NBT_VM_TEST_OK\n'
"@
$output = @($script.Replace("`r", '') | & ssh @options qnx-build "tr -d '\015' | sh -s")
$exitCode = $LASTEXITCODE
$output | Write-Output
if ($exitCode -ne 0 -or $output -notcontains 'NBT_VM_TEST_OK' -or $output -notcontains 'Hello World from QNX x86!') {
    throw 'Compilation or VM execution failed. No artifact accepted.'
}
$build = Join-Path $root ('build/' + $run)
New-Item -ItemType Directory -Path $build -Force | Out-Null
$binary = Join-Path $build 'hello_nbt'
# -O uses legacy SCP; QNX 6.5 need not provide a modern SFTP subsystem.
& scp -O @options "qnx-build:$remoteDir/hello_nbt" $binary
if ($LASTEXITCODE -ne 0) { throw 'Artifact download failed.' }
$elf = [IO.File]::ReadAllBytes($binary)
if ($elf.Length -lt 52 -or $elf[0] -ne 127 -or $elf[1] -ne 69 -or $elf[2] -ne 76 -or $elf[3] -ne 70 -or
    $elf[4] -ne 1 -or $elf[5] -ne 1 -or $elf[18] -ne 3 -or $elf[19] -ne 0) {
    throw 'Artifact is not ELF32 little-endian Intel 80386.'
}
$record = [ordered]@{
    builtUtc = [DateTime]::UtcNow.ToString('o')
    sourceSha256 = (Get-Sha256Hex (Join-Path $root 'src/hello_nbt.c'))
    binarySha256 = (Get-Sha256Hex $binary)
    compilerTarget = '4.4.2,gcc_ntox86'
    vmTestPassed = $true
    nbtTestPassed = $false
    remoteDirectory = $remoteDir
}
$record | ConvertTo-Json | Set-Content (Join-Path $build 'manifest.json') -Encoding UTF8
$output | Set-Content (Join-Path $build 'vm-test.txt') -Encoding UTF8
Write-Output "Verified VM artifact: $binary"
