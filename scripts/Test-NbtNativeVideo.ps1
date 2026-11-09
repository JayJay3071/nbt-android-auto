[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Stage', 'Run', 'Cleanup', 'Status')][string]$Action,
    [string]$Python,
    # Run: video device class and identifier exactly as found in the stock inventory.
    [ValidatePattern('^Nme[A-Za-z0-9]+$')][string]$Device,
    # URL-parameter style device identifier, e.g. '?video-window-class=X&video-layer-id=2'
    [ValidatePattern('^[A-Za-z0-9_=&?.:/,\-]*$')][string]$DeviceId = '',
    [ValidateRange(0, 64)][int]$Layer = -1,
    [ValidateRange(0, 4096)][int]$X = -1,
    [ValidateRange(0, 4096)][int]$Y = -1,
    [ValidateRange(0, 4096)][int]$Left = -1,
    [ValidateRange(0, 4096)][int]$Top = -1,
    [ValidateSet('pattern-800x480.mp4', 'pattern-800x480.h264')][string]$Clip = 'pattern-800x480.mp4',
    [ValidateRange(1, 20)][int]$Seconds = 8,
    [switch]$DryRun,
    # Run: loop the clip (-r) until the time limit; the pattern clip is only about 1 s long.
    [switch]$Repeat,
    # Overridable only so the tooling can be rehearsed against the build VM; defaults are the vehicle paths.
    # Cleanup runs 'rm -r' on this directory (the car has no rmdir), so it must live under RAM directories.
    [ValidatePattern('^/(fs/ram3|tmp)/[A-Za-z0-9_-][A-Za-z0-9_./-]*$')][string]$RemoteDir = '/fs/ram3/nbt-video-test',
    [ValidatePattern('^/[A-Za-z0-9_./-]+$')][string]$StockBin = '/opt/mm/bin/NmeCmdLine',
    [ValidatePattern('^/[A-Za-z0-9_./-]+$')][string]$StockCinemoDir = '/opt/mm/lib/cinemo'
)
# Staging and bounded test of a NATIVE video device through the stock Cinemo service (NmeCmdLine is only a
# client of the shared io-nme; the video device is created INSIDE io-nme). RAM only (/fs/ram3/nbt-video-test).
# Do NOT run 'Run' before the stock inventory (scripts/Collect-NbtVideoRoute.ps1) has been analysed:
# device class and identifier must be taken from the stock configuration, never guessed.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$remote = $RemoteDir
$artifacts = Join-Path $root 'build\native-video-test-20261106'
if (-not $Python) {
    $Python = if ($env:NBT_PYTHON) { $env:NBT_PYTHON } else { 'python' }
}
$request = Join-Path $root 'tools\nbt_request.py'
$statusFile = Join-Path $root '.local\ssh-session\status.json'
$evidence = Join-Path $root ('build\native-video-test-20261106\vehicle-' + (Get-Date -Format 'yyyyMMddTHHmmss'))
$libs = 'LD_LIBRARY_PATH=/opt/mm/lib:/opt/mm/lib/cinemo:/lib:/usr/lib'

function Send([string]$Command, [string]$InputFile, [string]$OutputFile, [int]$Timeout = 30) {
    if ($DryRun) { Write-Host "[dry-run] $Command"; if ($InputFile) { Write-Host "          stdin: $InputFile" }; return }
    $arguments = @('-I', $request, $Command, '--timeout', $Timeout)
    if ($InputFile) { $arguments += @('--input-file', $InputFile) }
    if ($OutputFile) { $arguments += @('--output-file', $OutputFile) }
    & $Python @arguments
    if ($LASTEXITCODE -ne 0) { throw "Remote command failed (exit $LASTEXITCODE). Not retried automatically: $Command" }
}
if (-not $DryRun) {
    $state = (Get-Content -LiteralPath $statusFile -Raw | ConvertFrom-Json).state
    if ($state -notin 'ready', 'busy') { throw "SSH session is '$state'. Start tools\nbt_ssh_session.py yourself first." }
}

switch ($Action) {
    'Status' {
        Send "ls -l $remote 2>&1; pidin ar | grep -e NmeCmdLine -e video_runner | grep -v grep; echo STATUS_END" $null $null
    }
    'Stage' {
        # mkdir fails if the directory already exists: nothing is overwritten.
        Send "mkdir $remote && chmod 700 $remote && cp $StockBin $StockCinemoDir/cinemo_classes.xml $StockCinemoDir/cinemo_links.xml $remote/ && ls -l $remote" $null $null
        foreach ($name in 'video_runner', 'pattern-800x480.mp4', 'pattern-800x480.h264') {
            $local = Join-Path $artifacts $name
            Send "cat > $remote/$name" $local $null 60
        }
        Send "chmod 700 $remote/video_runner $remote/NmeCmdLine; ls -l $remote" $null $null
        if (-not $DryRun) {
            New-Item -ItemType Directory -Force $evidence | Out-Null
            foreach ($name in 'video_runner', 'pattern-800x480.mp4', 'pattern-800x480.h264') {
                $back = Join-Path $evidence "$name.readback"
                Send "cat $remote/$name" $null $back 60
                $same = (Get-FileHash $back -Algorithm SHA256).Hash -eq (Get-FileHash (Join-Path $artifacts $name) -Algorithm SHA256).Hash
                Write-Host ("{0}: read-back identical = {1}" -f $name, $same)
                if (-not $same) { throw "Read-back mismatch for $name; do not run tests. Use -Action Cleanup." }
            }
        }
    }
    'Run' {
        if (-not $Device) { throw 'Run needs -Device (from the stock inventory).' }
        $client = "-a0 -wd 0 -wi 0 -wm 0 -vd $Device"
        if ($DeviceId) { $client += " -vi '$DeviceId'" }
        if ($Layer -ge 0) { $client += " -vl $Layer" }
        if ($X -ge 0) { $client += " -x $X" }
        if ($Y -ge 0) { $client += " -y $Y" }
        if ($Left -ge 0) { $client += " -xx $Left" }
        if ($Top -ge 0) { $client += " -yy $Top" }
        # -i is only "show timing information"; the clip is positional. Use an ABSOLUTE path: the file is opened by the
        # shared io-nme process, whose working directory is not ours (a relative path gave 0 decoded frames, 2026-11-06).
        if ($Repeat) { $client += ' -r' }
        $client += " -i $remote/$Clip"
        # mkdir is the lock: a stale lock (timed-out earlier run) blocks further runs until inspected.
        $command = "cd $remote && mkdir run.lock && { $libs ./video_runner $Seconds ./NmeCmdLine $client > run.log 2>&1; echo RUNNER_RC=`$?; rm -r run.lock; sed 80q run.log; }"
        Write-Host "Client arguments: $client"
        if (-not $DryRun) { New-Item -ItemType Directory -Force $evidence | Out-Null }
        Send $command $null (Join-Path $evidence 'run-output.txt') ($Seconds + 20)
        if (-not $DryRun -and (Test-Path (Join-Path $evidence 'run-output.txt'))) { Get-Content (Join-Path $evidence 'run-output.txt') }
    }
    'Cleanup' {
        # Removes the staged files, a stale lock and then the directory itself (RemoteDir is restricted to /fs/ram3 or /tmp).
        # The car has no rmdir and no head/printf/env: rm -r is the only way to remove a directory.
        Send "cd $remote && rm -f video_runner pattern-800x480.mp4 pattern-800x480.h264 NmeCmdLine cinemo_classes.xml cinemo_links.xml run.log; rm -r run.lock 2>/dev/null; cd / && rm -r $remote && echo CLEANUP_OK" $null $null
    }
}
