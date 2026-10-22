<#
.SYNOPSIS
    Milestone-2 test: host and guest instances exchange state over UDP loopback
    and the guest mirrors the host's controlled creature.

.DESCRIPTION
    Starts the host first and WAITS until its game is in the world (the intro
    movie only advances while its window is foreground; a run that started the
    guest too early left the host stuck in its intro). Then starts the guest,
    waits for its world, lets the pair run, and reads the plugin logs:
      - host: "drive: move ordered" lines (its test driver walking the character)
      - both: "net role=..." once a second, with sent/recv counters
      - guest: the same line carries "distance" and "applied"
    PASS: the guest received packets from the host and applied positions, and
    its last reported distance to the host is small.

    Deploy first: .\tools\deploy.ps1 -Preset debug
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 150,
    [int] $SyncSeconds = 90,
    [double] $MaxDistance = 2.0
)

$ErrorActionPreference = 'Stop'
$bin = 'E:\DAO-Coop-Test\bin_ship'
$exe = Join-Path $bin 'daorigins.exe'
$instanceRoot = Join-Path $env:LOCALAPPDATA 'DAOLib\instances'

if (-not (Test-Path $exe)) { throw "test install not found: $exe" }
if (Get-Process -Name 'daorigins' -ErrorAction SilentlyContinue) {
    throw 'daorigins already running; stop it first (.\tools\launch.ps1 -StopAll)'
}

function LogPath([string] $instance) {
    Join-Path $instanceRoot "$instance\BioWare\Dragon Age\daolib\daocoop.log"
}

function Focus-GameWindow($process, [int] $seconds = 20) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $process.Refresh()
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
            Add-Type -Namespace Win32 -Name User32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
'@ -ErrorAction SilentlyContinue
            [void][Win32.User32]::ShowWindow($process.MainWindowHandle, 9)  # SW_RESTORE
            [void][Win32.User32]::SetForegroundWindow($process.MainWindowHandle)
            return
        }
        Start-Sleep -Milliseconds 500
    }
}

function Wait-ForWorld($process, [string] $log, [int] $limitSeconds) {
    $deadline = (Get-Date).AddSeconds($limitSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($process.HasExited) { throw "game exited while waiting for the world: $log" }
        if (Test-Path $log) {
            $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
            if ($text -match 'game loaded:') { return $true }
        }
        Start-Sleep -Seconds 2
    }
    return $false
}

$env:DAOCOOP_AUTOLOAD = '1'
$hostProc = $null
$guestProc = $null
try {
    Write-Host '--- starting host ---'
    $env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'; $env:DAOCOOP_DRIVE = '1'
    $hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Focus-GameWindow $hostProc 25
    $hostLoaded = Wait-ForWorld $hostProc (LogPath 'host') $HostWaitSeconds
    Write-Host ("  host reached the world: {0}" -f $hostLoaded)
    if (-not $hostLoaded) {
        Write-Host '  host did not reach the world; last log lines:'
        Get-Content (LogPath 'host') -Tail 8 | ForEach-Object { Write-Host "    $_" }
    }

    Write-Host '--- starting guest ---'
    $env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'; $env:DAOCOOP_DRIVE = '0'
    $guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Focus-GameWindow $guestProc 25
    $guestLoaded = Wait-ForWorld $guestProc (LogPath 'guest') $GuestWaitSeconds
    Write-Host ("  guest reached the world: {0}" -f $guestLoaded)

    if (-not ($hostLoaded -and $guestLoaded)) { throw 'one of the two games never reached the world; see the logs' }

    Write-Host ("--- syncing for {0} s ---" -f $SyncSeconds)
    Start-Sleep -Seconds $SyncSeconds
}
finally {
    foreach ($p in @($hostProc, $guestProc)) {
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
    Start-Sleep -Seconds 3
}

Write-Host ''
Write-Host '=== results ==='
$verdict = $true
foreach ($name in 'host', 'guest') {
    $log = LogPath $name
    Write-Host "--- $name ---"
    if (-not (Test-Path $log)) { Write-Host '  no log'; $verdict = $false; continue }
    $lines = Get-Content $log
    $lastNet = $lines | Where-Object { $_ -match 'net role=' } | Select-Object -Last 1
    Write-Host ("  last net: {0}" -f ($lastNet -replace '^\[[^\]]+\] \[[^\]]+\] \[info\] ', ''))
    if ($name -eq 'host') {
        $moves = ($lines | Where-Object { $_ -match 'drive: move ordered' } | Measure-Object).Count
        Write-Host ("  drive orders issued: {0}" -f $moves)
        if ($moves -lt 1) { $verdict = $false }
        if ($lastNet -notmatch 'recv ([1-9][0-9]*)') { $verdict = $false }
    }
    else {
        if ($lastNet -match 'applied ([0-9]+)') {
            $applied = [int]$Matches[1]
            Write-Host ("  positions applied: {0}" -f $applied)
            if ($applied -lt 10) { $verdict = $false }
        }
        else { Write-Host '  no applied count in the last net line'; $verdict = $false }
        # With two different avatars the distance between them says nothing;
        # the mirror distance of the puppet is the sync error. One avatar (the
        # same id on both sides) is compared directly.
        $localId = $null
        $remoteId = $null
        if ($lastNet -match 'local=(0x[0-9a-f]+) remote=(0x[0-9a-f]+)') {
            $localId = $Matches[1]
            $remoteId = $Matches[2]
        }
        if ($localId -and $remoteId -and $localId -eq $remoteId) {
            if ($lastNet -match '\bdistance ([0-9.]+)') {
                $distance = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
                Write-Host ("  last distance (same avatar): {0}" -f $distance)
                if ($distance -gt $MaxDistance) { $verdict = $false }
            }
            else { Write-Host '  no distance in the last net line'; $verdict = $false }
        }
        else {
            if ($lastNet -match 'puppet_distance ([0-9.]+)') {
                $puppetDistance = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
                Write-Host ("  last puppet distance: {0}" -f $puppetDistance)
                if ($puppetDistance -gt $MaxDistance) { $verdict = $false }
            }
            else { Write-Host '  no puppet_distance in the last net line'; $verdict = $false }
        }
    }
}

Write-Host ''
Write-Host ("VERDICT: {0}" -f ($(if ($verdict) { 'PASS' } else { 'FAIL' })))
if (-not $verdict) { exit 1 }
