<#
.SYNOPSIS
    Milestone-5 test: the host's hostile creature's health and position are
    mirrored into the guest game.

.DESCRIPTION
    Host: spawns an ogre far from the player before the handoff save (so the
    guest's loaded world has the same creature under the same id), then, once
    the guest is connected, damages it on a schedule and puts its id, position
    and health in every state packet. The guest applies the health with the
    engine's own property setter and mirrors the position.

    This is the combat *state channel*, not a full fight: the damage is applied
    by the host plugin (a stand-in for the engine's combat), and combat AI,
    effects and death animations are not synced yet.

    PASS requires:
      - host: "combat: hostile spawned 0x...", at least 5 "combat: hostile ...
        health A -> B" lines
      - guest: joined (game loaded: slot 91), last net line names the same
        hostile id, its hostile_health equals the host's last value (within 1),
        and hostile_applied >= 5
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $SyncSeconds = 100,
    [int] $MinDamageTicks = 5
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
            if ((Get-Content $log -Raw -ErrorAction SilentlyContinue) -match $pattern) { return $true }
        }
        Start-Sleep -Seconds 2
    }
    return $false
}

$hostProc = $null
$guestProc = $null
try {
    Write-Host '--- starting host (world -> hostile -> save+serve) ---'
    $env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
    $env:DAOCOOP_COMBAT_TEST = '1'; $env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
    $env:DAOCOOP_DRIVE = '0'; $env:DAOCOOP_AUTOLOAD = '1'
    $hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Focus-GameWindow $hostProc 25
    $hostWorld = Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds
    Write-Host ("  host reached the world: {0}" -f $hostWorld)
    if (-not $hostWorld) { throw 'host never reached the world' }
    $spawned = Wait-ForLogMatch $hostProc (LogPath 'host') 'combat: hostile spawned' 60
    Write-Host ("  host hostile spawned: {0}" -f $spawned)
    Write-Host '--- starting guest (join -> load) ---'
    $env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
    $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
    $env:DAOCOOP_DRIVE = '0'; $env:DAOCOOP_AUTOLOAD = '0'
    $guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Focus-GameWindow $guestProc 25
    $served = Wait-ForLogMatch $hostProc (LogPath 'host') 'handoff: sent the slot folder' 120
    Write-Host ("  host served the save: {0}" -f $served)
    $joined = Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds
    Write-Host ("  guest joined: {0}" -f $joined)

    Write-Host ("--- syncing the hostile for {0} s ---" -f $SyncSeconds)
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
$hostLog = LogPath 'host'
$guestLog = LogPath 'guest'
if (-not (Test-Path $hostLog)) { Write-Host 'no host log'; $verdict = $false }
if (-not (Test-Path $guestLog)) { Write-Host 'no guest log'; $verdict = $false }
if ($verdict) {
    $hostLines = Get-Content $hostLog
    $guestLines = Get-Content $guestLog

    $spawnLine = $hostLines | Where-Object { $_ -match 'combat: hostile spawned (0x[0-9a-f]+)' } | Select-Object -Last 1
    $hostId = $null
    if ($spawnLine -match 'combat: hostile spawned (0x[0-9a-f]+)') { $hostId = $Matches[1] }
    Write-Host "host hostile id: $hostId"
    if (-not $hostId) { $verdict = $false }

    $damageLines = @($hostLines | Where-Object { $_ -match 'combat: hostile 0x[0-9a-f]+ health ([0-9.]+) -> ([0-9.]+)' })
    Write-Host ("host damage ticks: {0}" -f $damageLines.Count)
    if ($damageLines.Count -lt $MinDamageTicks) { $verdict = $false }
    $hostDamageFinal = $null
    $hostFirstHealth = $null
    if ($damageLines.Count -gt 0) {
        $last = $damageLines[-1]
        if ($last -match 'health [0-9.]+ -> ([0-9.]+)') {
            $hostDamageFinal = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
        }
        $first = $damageLines[0]
        if ($first -match 'health ([0-9.]+) ->') {
            $hostFirstHealth = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
            Write-Host "host hostile health (damage log): $hostFirstHealth -> $hostDamageFinal"
            if (-not ($hostDamageFinal -lt $hostFirstHealth)) { $verdict = $false }
        }
    }
    # The live value the host is sending is what the guest should match; the
    # damage log's last line can be one engine tick behind the death state.
    $hostFinalHealth = $hostDamageFinal
    $hostNet = $hostLines | Where-Object { $_ -match 'net role=host' } | Select-Object -Last 1
    if ($hostNet -match 'local_hostile_health ([0-9.]+)') {
        $hostFinalHealth = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
        Write-Host "host hostile health (live, sent to guest): $hostFinalHealth"
    }

    $guestNet = $guestLines | Where-Object { $_ -match 'net role=guest' } | Select-Object -Last 1
    Write-Host ("guest last net: {0}" -f ($guestNet -replace '^\[[^\]]+\] \[[^\]]+\] \[info\] ', ''))
    if ($guestNet -match 'hostile=(0x[0-9a-f]+)\s+hostile_health\s+([0-9.]+).*?hostile_applied\s+([0-9]+)') {
        $guestHostileId = $Matches[1]
        $guestHealth = [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture)
        $guestApplies = [int]$Matches[3]
        Write-Host "guest sees hostile $guestHostileId, health $guestHealth, applied $guestApplies"
        if ($hostId -and $guestHostileId -ne $hostId) { $verdict = $false }
        if ($null -ne $hostFinalHealth -and [math]::Abs($guestHealth - $hostFinalHealth) -gt 1.0) { $verdict = $false }
        if ($guestApplies -lt $MinDamageTicks) { $verdict = $false }
    }
    else { Write-Host 'guest net line has no hostile fields'; $verdict = $false }
}

Write-Host ''
Write-Host ("VERDICT: {0}" -f ($(if ($verdict) { 'PASS' } else { 'FAIL' })))
if (-not $verdict) { exit 1 }



