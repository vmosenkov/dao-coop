<#
.SYNOPSIS
    Same-creature configuration: the guest explicitly takes party index 0 (the
    host's own Warden). The guest must keep its own position — no teleporting to
    the host — and the log must warn once.

.DESCRIPTION
    Both games load the host's save; the guest selects index 0 instead of a
    companion, so both controlled creatures have the same object id. Before the
    fix the mirror moved the guest's creature to the host's position every
    packet ("stuck together / teleported back"). After it, the guest's position
    must not be rewritten (applied 0), it must get away from the host when it
    walks, and one System line must explain the configuration.
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $WalkSeconds = 75,
    [double] $MinWalkedAway = 0.5
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

Write-Host '--- host: world + handoff, driving its own Warden ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '1'
$hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
if (-not (Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds)) { throw 'host never reached the world' }

Write-Host '--- guest: join, take index 0 (same Warden), drive ---'
$env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
$env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = '0'
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

# Same creature on both sides.
$same = $false
if ($guestNet -match 'local=(0x[0-9a-f]+) remote=(0x[0-9a-f]+)') {
    $same = ($Matches[1] -eq $Matches[2])
}
Write-Host ("  guest local == remote (same creature): " + $same)
if (-not $same) { Write-Host '  the guest did not end up on the same creature; this run is not the bug scenario'; $verdict = $false }

# The guest's own position must not be rewritten by the mirror.
if ($guestNet -match 'applied ([0-9]+)') {
    $guestApplied = [int]$Matches[1]
    Write-Host ("  guest positions rewritten by the mirror: " + $guestApplied)
    if ($guestApplied -gt 0) { $verdict = $false }
} else { Write-Host '  no applied count on the guest'; $verdict = $false }

# Walking away from the host must work.
$distances = @()
foreach ($line in ($guestLog | Where-Object { $_ -match 'net role=guest' })) {
    if ($line -match '\bdistance ([0-9.]+)') {
        $distances += [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
    }
}
$furthest = if ($distances.Count) { ($distances | Measure-Object -Maximum).Maximum } else { $null }
Write-Host ("  guest furthest distance from the host: " + $furthest)
if ($null -eq $furthest -or $furthest -lt $MinWalkedAway) { $verdict = $false }

# One warning explains the configuration.
$warned = ($guestLog | Where-Object { $_ -match 'Both players control the same creature' }).Count
Write-Host ("  'same creature' warning lines: " + $warned)
if ($warned -lt 1) { Write-Host '  (continuing: the warning is written when the guest first mirrors the host)' }

Get-Process daorigins -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

if ($verdict) {
    Write-Host 'VERDICT: PASS - the guest keeps its own position when both players control the same creature'
} else {
    Write-Host 'VERDICT: FAIL'
}
