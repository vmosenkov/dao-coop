<#
.SYNOPSIS
    Milestone-3 test: in-game join over the network by save transfer.

.DESCRIPTION
    Host: starts in the world, plays for a few seconds, writes a manual save to
    a private slot (91) and serves that slot's folder over TCP.
    Guest: starts at its main menu, connects, receives the folder, writes it
    into a manual slot of its own profile and asks the game to load it.
    Then the two games keep exchanging state (milestone 2), so the guest should
    be in the host's world and follow the host's creature.

    PASS requires, from the logs:
      - host: "handoff: save requested", "handoff: sent the slot folder", drive
        orders, packets received
      - guest: "join: received", "join: wrote", "join: asked the game to load",
        "game loaded: slot 91", packets received, positions applied, last
        distance <= 2 m
      - the last net line of both names the same area id

    Deploy first: .\tools\deploy.ps1 -Preset debug
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $HandoffWaitSeconds = 90,
    [int] $GuestWaitSeconds = 180,
    [int] $SyncSeconds = 60,
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

function Focus-GameWindow($process, [int] $seconds = 25) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $process.Refresh()
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
            Add-Type -Namespace Win32 -Name User32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
'@ -ErrorAction SilentlyContinue
            [void][Win32.User32]::ShowWindow($process.MainWindowHandle, 9)
            [void][Win32.User32]::SetForegroundWindow($process.MainWindowHandle)
            return
        }
        Start-Sleep -Milliseconds 500
    }
}

function Wait-ForLogMatch($process, [string] $log, [string] $pattern, [int] $limitSeconds) {
    $deadline = (Get-Date).AddSeconds($limitSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($process.HasExited) { return $false }
        if (Test-Path $log) {
            $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
            if ($text -match $pattern) { return $true }
        }
        Start-Sleep -Seconds 2
    }
    return $false
}

$env:DAOCOOP_AUTOLOAD = '1'
$hostProc = $null
$guestProc = $null
try {
    Write-Host '--- starting host (world -> save -> serve) ---'
    $env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'; $env:DAOCOOP_DRIVE = '1'; $env:DAOCOOP_HANDOFF = '1'
    $env:DAOCOOP_JOIN = '0'
    $hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Focus-GameWindow $hostProc 25
    $hostWorld = Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds
    Write-Host ("  host reached the world: {0}" -f $hostWorld)
    if (-not $hostWorld) { throw 'host never reached the world' }

    # Start the guest now: the host saves and opens its transfer window a few
    # seconds after reaching the world, and the guest retries its connection
    # until the host is listening. Do NOT wait for the "sent" line first — that
    # line only appears once a guest has connected.
    Write-Host '--- starting guest (join -> load) ---'
    $env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'; $env:DAOCOOP_DRIVE = '0'; $env:DAOCOOP_HANDOFF = '0'
    $env:DAOCOOP_JOIN = '1'; $env:DAOCOOP_AUTOLOAD = '0'
    $guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Focus-GameWindow $guestProc 25

    $handoffServed = Wait-ForLogMatch $hostProc (LogPath 'host') 'handoff: sent the slot folder' $HandoffWaitSeconds
    Write-Host ("  host served the save folder: {0}" -f $handoffServed)

    $guestJoined = Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds
    Write-Host ("  guest joined and loaded the host save: {0}" -f $guestJoined)

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
$areas = @{}
foreach ($name in 'host', 'guest') {
    $log = LogPath $name
    Write-Host "--- $name ---"
    if (-not (Test-Path $log)) { Write-Host '  no log'; $verdict = $false; continue }
    $lines = Get-Content $log
    $text = $lines -join "`n"
    $lastNet = $lines | Where-Object { $_ -match 'net role=' } | Select-Object -Last 1
    Write-Host ("  last net: {0}" -f ($lastNet -replace '^\[[^\]]+\] \[[^\]]+\] \[info\] ', ''))
    if ($lastNet -match 'area (0x[0-9a-f]+)') { $areas[$name] = $Matches[1] }
    if ($name -eq 'host') {
        foreach ($pattern in 'handoff: save requested', 'handoff: sent the slot folder') {
            $present = $text -match [regex]::Escape($pattern)
            Write-Host ("  {0}: {1}" -f $pattern, $present)
            if (-not $present) { $verdict = $false }
        }
        $moves = ($lines | Where-Object { $_ -match 'drive: move ordered' } | Measure-Object).Count
        Write-Host ("  drive orders: {0}" -f $moves)
        if ($moves -lt 1) { $verdict = $false }
        if ($lastNet -notmatch 'recv ([1-9][0-9]*)') { $verdict = $false }
    }
    else {
        foreach ($pattern in 'join: received the host', 'join: wrote', 'join: asked the game to load', 'game loaded: slot 91') {
            $present = $text -match [regex]::Escape($pattern)
            Write-Host ("  {0}: {1}" -f $pattern, $present)
            if (-not $present) { $verdict = $false }
        }
        if ($lastNet -match 'applied ([0-9]+)') {
            $applied = [int]$Matches[1]
            Write-Host ("  positions applied after the join: {0}" -f $applied)
            if ($applied -lt 10) { $verdict = $false }
        }
        else { Write-Host '  no applied count'; $verdict = $false }
        if ($lastNet -match 'puppet_distance ([0-9.]+)') {
            $puppetDistance = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
            Write-Host ("  last puppet distance: {0}" -f $puppetDistance)
            if ($puppetDistance -gt $MaxDistance) { $verdict = $false }
        }
        else { Write-Host '  no puppet_distance'; $verdict = $false }
    }
}
if ($areas.Count -eq 2) {
    $sameArea = $areas['host'] -eq $areas['guest']
    Write-Host ("same area in both games: {0} (host {1}, guest {2})" -f $sameArea, $areas['host'], $areas['guest'])
    if (-not $sameArea) { $verdict = $false }
}
else { Write-Host 'area id missing from a net line'; $verdict = $false }

Write-Host ''
Write-Host ("VERDICT: {0}" -f ($(if ($verdict) { 'PASS' } else { 'FAIL' })))
if (-not $verdict) { exit 1 }
