<#
.SYNOPSIS
    The bug scenario itself: the host and the guest walk in OPPOSITE directions
    at the same time. Both must keep their own position (applied 0), get far
    apart, and each game's copy of the other must follow its owner.

.DESCRIPTION
    Host serves the handoff save and walks -X; the guest joins and walks +X
    (DAOCOOP_DRIVE_AWAY). Before the fix the guest's creature was teleported to
    the host's position every packet, so the distance stayed near zero and the
    guest's `applied` grew. A pass needs both sides' `applied` 0 (no locally
    controlled creature rewritten), a furthest distance well above the room's
    noise, and small puppet distances (the copies follow).
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $WalkSeconds = 75,
    [double] $MinApart = 1.0,
    [double] $MaxPuppetDistance = 2.0
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

Write-Host '--- host: world + handoff, walking one way ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'
$env:DAOCOOP_DRIVE = '1'; $env:DAOCOOP_DRIVE_AWAY = '1'
$hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
if (-not (Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds)) { throw 'host never reached the world' }

Write-Host '--- guest: join, walk the other way ---'
$env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
$env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'
$env:DAOCOOP_DRIVE = '1'; $env:DAOCOOP_DRIVE_AWAY = '1'
$guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
$joined = Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds
Write-Host ("  guest joined: {0}" -f $joined)
& (Join-Path $PSScriptRoot 'windowed.ps1') -Arrange -Width 960 -Height 540 2>&1 | ForEach-Object { Write-Host "  $_" }

Write-Host ("--- walking apart for {0} s ---" -f $WalkSeconds)
Start-Sleep -Seconds $WalkSeconds

$verdict = $true
$hostLog = Get-Content (LogPath 'host')
$guestLog = Get-Content (LogPath 'guest')
foreach ($name in 'host', 'guest') {
    $log = if ($name -eq 'host') { $hostLog } else { $guestLog }
    $net = $log | Where-Object { $_ -match "net role=$name" } | Select-Object -Last 1
    Write-Host ("  {0} last net: {1}" -f $name, ($net -replace '^\[[^\]]+\] \[[^\]]+\] \[info\] ', ''))
    if ($net -match 'applied ([0-9]+)') {
        $applied = [int]$Matches[1]
        Write-Host ("  {0} own positions rewritten by the mirror: {1}" -f $name, $applied)
        if ($applied -gt 0) { $verdict = $false }
    }
    else { Write-Host ("  no applied count on the {0}" -f $name); $verdict = $false }
    if ($net -match 'puppet_distance ([0-9.]+)') {
        $puppet = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
        Write-Host ("  {0} puppet distance: {1}" -f $name, $puppet)
        if ($puppet -gt $MaxPuppetDistance) { $verdict = $false }
    }
}

$distances = @()
foreach ($line in ($guestLog | Where-Object { $_ -match 'net role=guest' })) {
    if ($line -match '\bdistance ([0-9.]+)') {
        $distances += [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
    }
}
$furthest = if ($distances.Count) { ($distances | Measure-Object -Maximum).Maximum } else { $null }
$first = if ($distances.Count) { $distances[0] } else { $null }
Write-Host ("  distance first -> furthest: {0} -> {1}" -f $first, $furthest)
if ($null -eq $furthest -or $furthest -lt $MinApart) { $verdict = $false }
if ($null -eq $first -or ($furthest - $first) -lt 0.5) { $verdict = $false }

$hostDrives = ($hostLog | Where-Object { $_ -match 'drive: move ordered' }).Count
$guestDrives = ($guestLog | Where-Object { $_ -match 'drive: move ordered' }).Count
Write-Host ("  drive orders: host {0}, guest {1}" -f $hostDrives, $guestDrives)
if ($hostDrives -lt 2 -or $guestDrives -lt 2) { $verdict = $false }

Get-Process daorigins -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

if ($verdict) {
    Write-Host 'VERDICT: PASS - the players walked apart and neither was teleported back'
} else {
    Write-Host 'VERDICT: FAIL'
}

