[CmdletBinding()]
param([Parameter(Mandatory)][string]$VerifiedFingerprint)
$ErrorActionPreference = 'Stop'
# Expected fingerprint must come from the local VM console, never from the network.
if ($VerifiedFingerprint -match '^MD5:([0-9a-f]{2}:){15}[0-9a-f]{2}$') { $hashMode = 'md5' }
elseif ($VerifiedFingerprint -match '^SHA256:[A-Za-z0-9+/]{43}$') { $hashMode = 'sha256' }
else { throw 'Supply MD5:... or SHA256:... verified at the VM console.' }
$root = Split-Path $PSScriptRoot -Parent
$local = Join-Path $root '.local'
New-Item -ItemType Directory -Force -Path $local | Out-Null
$known = Join-Path $local 'known_hosts'
if (Test-Path -LiteralPath $known) { throw 'known_hosts already exists; review changes manually.' }
$pending = Join-Path $local ('hostkey-' + [Guid]::NewGuid().ToString('N') + '.pending')
$node = (Get-Command node -ErrorAction Stop).Source.Replace('\','/')
$helper = (Join-Path $PSScriptRoot 'verify-host-key.cjs').Replace('\','/')
$command = '"{0}" "{1}" "{2}" "{3}" %H %t %K' -f $node,$helper,$VerifiedFingerprint,$pending.Replace('\','/')
try {
    # OpenSSH 5.2 on QNX needs a MAC override unavailable in modern ssh-keyscan.
    # KnownHostsCommand emits the key only AFTER checking the expected fingerprint.
    # No password/private-key authentication is attempted during this probe.
    & ssh -F NUL -o HostKeyAlgorithms=+ssh-rsa -o MACs=+hmac-sha1 `
        -o BatchMode=yes -o PreferredAuthentications=none -o PubkeyAuthentication=no `
        -o PasswordAuthentication=no -o KbdInteractiveAuthentication=no `
        -o StrictHostKeyChecking=yes -o "UserKnownHostsFile=$pending" `
        -o GlobalKnownHostsFile=none -o "KnownHostsCommand=$command" `
        -o ConnectTimeout=5 root@192.168.56.10 true 2>$null
    # A denied login is expected. Only a successfully verified host key matters.
    if (!(Test-Path -LiteralPath $pending)) { throw 'Could not verify VM host key.' }
    $details = & ssh-keygen -E $hashMode -lf $pending
    if ($LASTEXITCODE -ne 0) { throw 'Cannot calculate host fingerprint.' }
    if (($details -split '\s+')[1] -cne $VerifiedFingerprint) { throw 'VM host key mismatch.' }
    Move-Item -LiteralPath $pending -Destination $known
    Write-Output 'Verified VM host key saved to .local/known_hosts.'
} finally {
    if (Test-Path -LiteralPath $pending) { Remove-Item -LiteralPath $pending }
}