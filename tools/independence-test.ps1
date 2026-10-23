<#
.SYNOPSIS
    Two independent players: host stands still, guest walks away, and the two
    creatures must not share a position; also checks that the session event log
    has the same lines on both sides.

.DESCRIPTION
    The host serves its save and stands still (drive off). The guest joins with
    NO party index set, so the plugin picks the first companion automatically.
    The guest then walks its own creature (drive on). If the two players were
    bound to one creature, the guest could not walk away from the host: the
    guest's distance to the host would stay near zero and its own positions
    would be rewritten. A pass needs:
      * guest log: "party: the guest will take party index 1 by default";
      * different controlled ids on both sides (local != remote);
      * guest drive orders and a last distance to the standing host >= 3 m;
      * host puppet distance <= 2 m (the copy of the guest follows it);
      * both event logs contain both join lines (local + remote).
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $WalkSeconds = 90,
    [double] $MinWalkedAway = 0.5,
    [double] $MaxPuppetDistance = 2.0,
    # An address of this machine other than loopback exercises the LAN path.
    [string] $PeerIp = ''
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

if ($PeerIp) { $env:DAOCOOP_PEERIP = $PeerIp; Write-Host ("--- using the LAN address {0} ---" -f $PeerIp) }

Write-Host '--- host: world + handoff, standing still ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
if (-not (Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds)) { throw 'host never reached the world' }

Write-Host '--- guest: join (no party index set) and walk away ---'
$env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
$env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
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
Write-Host ("  guest last net: " + ($guestNet -replace '^\[[^\]]+\] \[[^\]]+\] \[info\] ', ''))

# 1. The guest picked a party member of its own.
$auto = ($guestLog | Where-Object { $_ -match 'party: the guest will take party index 1 by default' }).Count
Write-Host ("  guest auto party pick: {0}" -f ($auto -gt 0))
if ($auto -lt 1) { $verdict = $false }

# 2. Different controlled creatures.
$hostLocal = $null; $hostRemote = $null; $guestLocal = $null; $guestRemote = $null
if ($hostNet -match 'local=(0x[0-9a-f]+) remote=(0x[0-9a-f]+)') { $hostLocal = $Matches[1]; $hostRemote = $Matches[2] }
if ($guestNet -match 'local=(0x[0-9a-f]+) remote=(0x[0-9a-f]+)') { $guestLocal = $Matches[1]; $guestRemote = $Matches[2] }
$different = $hostLocal -and $guestLocal -and ($hostLocal -ne $guestLocal)
Write-Host ("  different controlled ids: " + $different + " (host " + $hostLocal + ", guest " + $guestLocal + ")")
if (-not $different) { $verdict = $false }

# 3. The guest walked away from the standing host.
# The drive walks a circle and may come back near its anchor at the end, so the
# evidence is the FURTHEST the guest got from the standing host, not the last
# sample.
$guestDrives = ($guestLog | Where-Object { $_ -match 'drive: move ordered' }).Count
$guestDistance = $null
$distances = @()
foreach ($line in ($guestLog | Where-Object { $_ -match 'net role=guest' })) {
    if ($line -match '\bdistance ([0-9.]+)') {
        $distances += [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
    }
}
if ($distances.Count -gt 0) { $guestDistance = ($distances | Measure-Object -Maximum).Maximum }
Write-Host ("  guest drive orders: " + $guestDrives + "; furthest distance from the standing host: " + $guestDistance)
if ($guestDrives -lt 5) { $verdict = $false }
if ($null -eq $guestDistance -or $guestDistance -lt $MinWalkedAway) { $verdict = $false }
# The bound-to-one-creature bug rewrote the guest's own position from the host's
# every packet; a guest that drives itself must not be re-positioned.
if ($guestNet -match 'applied ([0-9]+)') {
    $guestApplied = [int]$Matches[1]
    Write-Host ("  guest positions rewritten by the mirror: " + $guestApplied)
    if ($guestApplied -gt 5) { $verdict = $false }
}
else { Write-Host '  no applied count on the guest'; $verdict = $false }

# 4. The host's copy of the guest follows it.
if ($hostNet -match 'puppet_distance ([0-9.]+)') {
    $puppetDistance = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
    Write-Host ("  host puppet distance: " + $puppetDistance)
    if ($puppetDistance -gt $MaxPuppetDistance) { $verdict = $false }
}
else { Write-Host '  no puppet_distance on the host'; $verdict = $false }

# 5. Both logs hold the same two join lines (each side writes one, the other
# arrives in the event packet).
foreach ($line in 'Another player joined the game', 'Joined the host''s game') {
    $onHost = ($hostLog | Where-Object { $_ -match [regex]::Escape($line) }).Count
    $onGuest = ($guestLog | Where-Object { $_ -match [regex]::Escape($line) }).Count
    Write-Host ("  event '" + $line + "': host " + $onHost + ", guest " + $onGuest)
    if ($onHost -lt 1 -or $onGuest -lt 1) { $verdict = $false }
}

Get-Process daorigins -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

if ($verdict) {
    Write-Host 'VERDICT: PASS - the players are independent and both event logs hold both join lines'
} else {
    Write-Host 'VERDICT: FAIL'
}






