<#
.SYNOPSIS
    The guest walks; the standing host must not move. This is the regression for
    the party group move (dao::orders::MoveTo) that used to drag the controlled
    creature of the other game toward the walker.

.DESCRIPTION
    The host stays put (no drive); the guest drives its own creature around a
    circle. The host's copy of the guest must walk (puppet_walks), the guest's
    copy of the host must stand near the host, and the host's own position must
    stay where it was (a group move would have shifted it). Both `applied`
    counters must stay 0.
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $WalkSeconds = 65,
    [double] $MaxHostDrift = 0.5,
    [int] $MinWalks = 4,
    [double] $MaxPuppetDistance = 2.0,
    [double] $MinGuestTravel = 1.0
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

function ParsePositions([string[]] $lines, [string] $role) {
    $positions = @()
    foreach ($line in $lines) {
        if ($line -match "net role=$role .*local \(([-0-9.]+), ([-0-9.]+), ([-0-9.]+)\)") {
            $positions += ,@(
                [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture),
                [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture)
            )
        }
    }
    return $positions
}

Write-Host '--- host: world + handoff, standing still ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
if (-not (Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds)) { throw 'host never reached the world' }

Write-Host '--- guest: join, select party 1, walk ---'
$env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
$env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = '1'
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '1'
$guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
$joined = Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds
Write-Host ("  guest joined: {0}" -f $joined)
& (Join-Path $PSScriptRoot 'windowed.ps1') -Arrange -Width 960 -Height 540 2>&1 | ForEach-Object { Write-Host "  $_" }

Write-Host ("--- walking for {0} s ---" -f $WalkSeconds)
Start-Sleep -Seconds $WalkSeconds

$verdict = $true
$hostLog = Get-Content (LogPath 'host')
$guestLog = Get-Content (LogPath 'guest')
$hostNet = $hostLog | Where-Object { $_ -match 'net role=host' } | Select-Object -Last 1
$guestNet = $guestLog | Where-Object { $_ -match 'net role=guest' } | Select-Object -Last 1
Write-Host ("  host last net: {0}" -f ($hostNet -replace '^\[[^\]]+\] \[[^\]]+\] \[info\] ', ''))
Write-Host ("  guest last net: {0}" -f ($guestNet -replace '^\[[^\]]+\] \[[^\]]+\] \[info\] ', ''))

# The host's own position must not drift while the guest walks.
$hostPositions = @(ParsePositions $hostLog 'host')
if ($hostPositions.Count -ge 2) {
    $x0 = $hostPositions[0][0]; $y0 = $hostPositions[0][1]
    $x1 = $hostPositions[-1][0]; $y1 = $hostPositions[-1][1]
    $drift = [math]::Sqrt((($x1 - $x0) * ($x1 - $x0)) + (($y1 - $y0) * ($y1 - $y0)))
    Write-Host ("  host own drift: {0:N2} m (from {1:N2},{2:N2} to {3:N2},{4:N2})" -f $drift, $x0, $y0, $x1, $y1)
    if ($drift -gt $MaxHostDrift) { $verdict = $false }
} else {
    Write-Host '  not enough host samples'; $verdict = $false
}

# The guest must move; both applied counters must stay 0.
$guestPositions = @(ParsePositions $guestLog 'guest')
if ($guestPositions.Count -ge 2) {
    $gx0 = $guestPositions[0][0]; $gy0 = $guestPositions[0][1]
    $gx1 = $guestPositions[-1][0]; $gy1 = $guestPositions[-1][1]
    $travel = [math]::Sqrt((($gx1 - $gx0) * ($gx1 - $gx0)) + (($gy1 - $gy0) * ($gy1 - $gy0)))
    Write-Host ("  guest own travel: {0:N2} m" -f $travel)
    if ($travel -lt $MinGuestTravel) { $verdict = $false }
} else {
    Write-Host '  not enough guest samples'; $verdict = $false
}
foreach ($pair in @(@('host', $hostNet), @('guest', $guestNet))) {
    $name = $pair[0]; $net = $pair[1]
    if ($net -match 'applied ([0-9]+)') {
        $applied = [int]$Matches[1]
        Write-Host ("  {0} positions rewritten by the mirror: {1}" -f $name, $applied)
        if ($applied -gt 0) { $verdict = $false }
    } else { $verdict = $false }
}

# The host's copy of the guest must walk (a script command, not a teleport-only path).
$hostWalks = 0
$guestWalks = 0
if ($hostNet -match 'puppet_walks ([0-9]+)') { $hostWalks = [int]$Matches[1] }
if ($guestNet -match 'puppet_walks ([0-9]+)') { $guestWalks = [int]$Matches[1] }
Write-Host ("  puppet walks: host {0}, guest {1}" -f $hostWalks, $guestWalks)
if ($hostWalks -lt $MinWalks) { $verdict = $false }

foreach ($pair in @(@('host', $hostNet), @('guest', $guestNet))) {
    if ($pair[1] -match 'puppet_distance ([0-9.]+)') {
        $distance = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
        Write-Host ("  {0} puppet distance: {1:N2}" -f $pair[0], $distance)
        if ($distance -gt $MaxPuppetDistance) { $verdict = $false }
    }
}

Get-Process daorigins -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

if ($verdict) {
    Write-Host 'VERDICT: PASS - the guest walks, the host stays put; the copies follow'
} else {
    Write-Host 'VERDICT: FAIL'
}
