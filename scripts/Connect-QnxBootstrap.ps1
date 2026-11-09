# Run interactively on Windows. ssh reads the password from the console, never from this script.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$status = Join-Path $root '.local/bootstrap-status.json'
try {
    $config = Join-Path $root '.local/ssh_config'
    $known = Join-Path $root '.local/known_hosts'
    $key = Join-Path $env:LOCALAPPDATA 'NBT-Project/ssh/qnx_vm_rsa.pub'
    if (!(Test-Path $config) -or !(Test-Path $known) -or !(Test-Path $key)) { throw 'Verified SSH config and dedicated public key are required.' }
    $public = [IO.File]::ReadAllText($key).Trim()
    if ($public -notmatch '^ssh-rsa ([A-Za-z0-9+/=]+)(?: .*)?$') { throw 'Unexpected public key format.' }
    $entry = 'from="192.168.56.1",no-agent-forwarding,no-port-forwarding,no-X11-forwarding,no-pty ssh-rsa ' + $Matches[1] + ' nbt-qnx-vm-build'
    $remote = @"
set -eu
test "`$(uname -s)" = QNX
test "`$(uname -r)" = 6.5.0
umask 077
cd "`$HOME"
mkdir -p .ssh
chmod 700 .ssh
touch .ssh/authorized_keys
chmod 600 .ssh/authorized_keys
entry='$entry'
if grep -F -x "`$entry" .ssh/authorized_keys >/dev/null 2>&1; then
    :
else
    printf '%s\n' "`$entry" >> .ssh/authorized_keys
fi
printf 'NBT_KEY_READY\n'
"@
    $remote = $remote.Replace("`r", '')
    $options = @('-F',$config,'-o',"UserKnownHostsFile=$known",'-o','GlobalKnownHostsFile=none',
        '-o','StrictHostKeyChecking=yes','-o','ClearAllForwardings=yes','-o','ForwardAgent=no',
        '-o','ForwardX11=no','-o','PermitLocalCommand=no','-o','ProxyCommand=none','-o','ProxyJump=none')
    $effective = & ssh @options -G qnx-build
    if ($LASTEXITCODE -ne 0 -or $effective -notcontains 'hostname 192.168.56.10' -or $effective -notcontains 'port 22') { throw 'Only the verified development VM is allowed.' }
    Write-Host 'QNX VM SSH setup: enter the password for the configured build user (nbtbuild).' 
    Write-Host 'Password input is invisible and is not recorded. No transcript is enabled.'
    $remote | & ssh @options -o BatchMode=no -o PubkeyAuthentication=no -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1 qnx-build "tr -d '\015' | sh -s" 2>&1 | Tee-Object -FilePath (Join-Path $root '.local/bootstrap-result.txt')
    if ($LASTEXITCODE -ne 0) { throw 'SSH key installation failed. No password was stored.' }
    & ssh @options -o BatchMode=yes -o PreferredAuthentications=publickey qnx-build 'id' 2>&1 | Tee-Object -FilePath (Join-Path $root '.local/bootstrap-result.txt') -Append
    if ($LASTEXITCODE -ne 0) { throw 'Public key verification failed.' }
    @{ state='ready'; completedUtc=[DateTime]::UtcNow.ToString('o') } | ConvertTo-Json | Set-Content $status -Encoding utf8
    Write-Host 'SSH key authentication is ready. You may close this window.' -ForegroundColor Green
} catch {
    @{ state='failed'; reason=$_.Exception.Message; completedUtc=[DateTime]::UtcNow.ToString('o') } | ConvertTo-Json | Set-Content $status -Encoding utf8
    Write-Host $_.Exception.Message -ForegroundColor Red
}
[void](Read-Host 'Press Enter to close')