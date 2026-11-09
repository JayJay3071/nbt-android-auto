[CmdletBinding()]
param([switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$known = Join-Path $root '.local/known_hosts'
$expected = 'SHA256:LrZmBXlAHJWOqKwQmPCQTLIVaEY3wbUAEjnMEDUsOew'
foreach ($program in @('ssh.exe','scp.exe','ssh-keygen.exe')) {
    Get-Command $program -ErrorAction Stop | Out-Null
}
if (!(Test-Path -LiteralPath $known)) { throw 'Verified VM host key missing. Restore it from the migration package.' }
$fingerprint = & ssh-keygen -E sha256 -lf $known
if ($LASTEXITCODE -ne 0 -or ($fingerprint -split '\s+')[1] -cne $expected) {
    throw 'VM host key does not match the verified migration identity.'
}
if ($CheckOnly) { Write-Output 'OpenSSH tools and migrated VM host key verified. No changes made.'; return }
$keyDir = Join-Path $env:LOCALAPPDATA 'NBT-Project/ssh'
$key = Join-Path $keyDir 'qnx_vm_rsa'
if (!(Test-Path -LiteralPath $key)) {
    New-Item -ItemType Directory -Path $keyDir -Force | Out-Null
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    & icacls.exe $keyDir /inheritance:r /grant:r ('*'+$sid+':(OI)(CI)F') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot restrict private key directory permissions.' }
    # Start-Process preserves the empty passphrase argument on Windows PowerShell 5.1.
    $keygen = (Get-Command ssh-keygen.exe).Source
    $arguments = '-q -t rsa -b 3072 -N "" -C nbt-qnx-laptop -f "' + $key + '"'
    $process = Start-Process -FilePath $keygen -ArgumentList $arguments -Wait -PassThru -NoNewWindow
    if ($process.ExitCode -ne 0) { throw 'SSH key generation failed.' }
}
if (!(Test-Path -LiteralPath ($key + '.pub'))) { throw 'Public key missing; existing private key was not overwritten.' }
$configPath = Join-Path $root '.local/ssh_config'
if (Test-Path -LiteralPath $configPath) {
    throw 'SSH config already exists. It was not overwritten. Use Connect-QnxBootstrap.ps1 if appropriate.'
}
$keyUnix = $key.Replace('\','/')
$config = @"
Host qnx-build
    HostName 192.168.56.10
    User nbtbuild
    Port 22
    IdentityFile "$keyUnix"
    IdentitiesOnly yes
    StrictHostKeyChecking yes
    ForwardAgent no
    ForwardX11 no
    ClearAllForwardings yes
    ConnectTimeout 5
    ServerAliveInterval 15
    ServerAliveCountMax 2
    HostKeyAlgorithms +ssh-rsa
    PubkeyAcceptedAlgorithms +ssh-rsa
    MACs +hmac-sha1
"@
[IO.File]::WriteAllText($configPath, $config, [Text.UTF8Encoding]::new($false))
Write-Output 'Laptop key and VM SSH config prepared. Enter the nbtbuild password in the local prompt.'
& (Join-Path $PSScriptRoot 'Connect-QnxBootstrap.ps1')
$statusPath = Join-Path $root '.local/bootstrap-status.json'
if (!(Test-Path $statusPath) -or (Get-Content $statusPath -Raw | ConvertFrom-Json).state -ne 'ready') {
    throw 'SSH setup is incomplete. See the local bootstrap result and retry once the VM is ready.'
}