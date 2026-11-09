[CmdletBinding()]
param(
    # Python that can run tools\nbt_request.py (standard library only). Default: $env:NBT_PYTHON, else python.
    [string]$Python,
    [ValidateSet('procs','libs','devs','graphics','cfg-cinemo','cfg-mme','cfg-generic','cfg-search','use','slog')]
    [string[]]$Sections = @('procs','libs','devs','graphics','cfg-cinemo','cfg-mme','cfg-generic','cfg-search','use','slog'),
    [string]$OutDir,
    # Print what would be sent, do not touch the vehicle.
    [switch]$DryRun
)
# Read-only vehicle inventory of the stock video route. Runs scripts/Inspect-NbtVideoRoute.sh section by
# section through the already-open SSH session (tools\nbt_ssh_session.py). Never retries automatically.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Python) {
    $Python = if ($env:NBT_PYTHON) { $env:NBT_PYTHON } else { 'python' }
}
$request = Join-Path $root 'tools\nbt_request.py'
$script = Join-Path $root 'scripts\Inspect-NbtVideoRoute.sh'
$status = Join-Path $root '.local\ssh-session\status.json'
if (-not $OutDir) { $OutDir = Join-Path $root ('build\native-video-route-20261106\vehicle-' + (Get-Date -Format 'yyyyMMddTHHmmss')) }
if (-not $DryRun) {
    $state = (Get-Content -LiteralPath $status -Raw | ConvertFrom-Json).state
    if ($state -notin 'ready', 'busy') {
        throw "SSH session is '$state', not ready. In your own terminal run: python tools\nbt_ssh_session.py (type the password yourself)."
    }
    New-Item -ItemType Directory -Force $OutDir | Out-Null
}
foreach ($section in $Sections) {
    $command = "tr -d '\015' | sh -s -- $section"
    $out = Join-Path $OutDir "$section.txt"
    Write-Host "== $section -> $out"
    if ($DryRun) { Write-Host "   [dry-run] $command  (stdin: $script)"; continue }
    & $Python -I $request $command --input-file $script --timeout 40 --output-file $out
    if ($LASTEXITCODE -ne 0) { Write-Warning "$section failed (exit $LASTEXITCODE); not retried automatically." }
}
if (-not $DryRun) { Write-Host "Done. Evidence directory: $OutDir" }
