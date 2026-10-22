<#
.SYNOPSIS
    Combat arbitration test: the guest's attack becomes a request, the host
    validates and executes it, and the host's answer comes back.

.DESCRIPTION
    Starts the host (world -> ogre -> handoff save) and the guest (join by save
    transfer), then watches both logs for the full chain:

        guest:  attack: request N sent (attacker 0x..., target 0x...)
        host:   attack: guest request N attacker 0x... target 0x... -> accepted
        guest:  attack: result for request N ... : accepted

    The guest turns its own attack order into the request (commands::OnStart);
    the host runs dao::orders::Attack so DAO itself resolves the fight. The
    resulting health comes back in the host's hostile table, which the guest
    mirrors (checked by demo-fight.ps1 / combat-test.ps1).

    Prints VERDICT: PASS|FAIL. Windows are closed unless -KeepOpen.
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $FightWaitSeconds = 120,
    [switch] $KeepOpen = $false
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
        if ($process.HasExited) { return $null }
        if (Test-Path $log) {
            $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
            if ($text -match $pattern) { return $Matches }
        }
        Start-Sleep -Seconds 2
    }
    return $null
}

Write-Host '--- host: world, ogre, handoff ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '1'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '1'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Focus-GameWindow $hostProc 25
if (-not (Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds)) { throw 'host never reached the world' }

Write-Host '--- guest: join ---'
$env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
$env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = '1'
$env:DAOCOOP_DEMO_FIGHT = '1'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Focus-GameWindow $guestProc 25
$joined = Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds
Write-Host ("  guest joined: {0}" -f ($null -ne $joined))

Start-Sleep -Seconds 4
& (Join-Path $PSScriptRoot 'windowed.ps1') -Arrange -Width 960 -Height 540 2>&1 | ForEach-Object { Write-Host "  $_" }

Write-Host '--- waiting for the fight and the attack request ---'
$fight = Wait-ForLogMatch $hostProc (LogPath 'host') 'demo: fight started' $FightWaitSeconds
Write-Host ("  host started the fight: {0}" -f ($null -ne $fight))

$hostRequest = Wait-ForLogMatch $hostProc (LogPath 'host') 'attack: guest request (\d+) attacker (0x[0-9a-f]+) target (0x[0-9a-f]+) -> accepted' $FightWaitSeconds
$guestSent = Wait-ForLogMatch $guestProc (LogPath 'guest') 'attack: request (\d+) sent \(attacker (0x[0-9a-f]+), target (0x[0-9a-f]+)\)' $FightWaitSeconds
$guestResult = Wait-ForLogMatch $guestProc (LogPath 'guest') 'attack: result for request (\d+) \(attacker (0x[0-9a-f]+), target (0x[0-9a-f]+)\): accepted' $FightWaitSeconds

$verdict = $true
Write-Host ''
Write-Host '=== results ==='
if ($null -eq $hostRequest) { Write-Host 'host never logged a guest attack request'; $verdict = $false }
if ($null -eq $guestSent) { Write-Host 'guest never logged sending a request'; $verdict = $false }
if ($null -eq $guestResult) { Write-Host 'guest never logged an attack result'; $verdict = $false }

if ($null -ne $hostRequest) { Write-Host ("host accepted request {0}: attacker {1} target {2}" -f $hostRequest[1], $hostRequest[2], $hostRequest[3]) }
if ($null -ne $guestSent) { Write-Host ("guest sent request {0}: attacker {1} target {2}" -f $guestSent[1], $guestSent[2], $guestSent[3]) }
if ($null -ne $guestResult) { Write-Host ("guest result request {0}: attacker {1} target {2} -> accepted" -f $guestResult[1], $guestResult[2], $guestResult[3]) }

# The first request may be refused while the host has not seen the guest's
# first state packet; the guest then repeats the same id. So guestSent is
# informational, and the accepted request must match the accepted result.
if ($null -ne $hostRequest -and $null -ne $guestResult) {
    if ($hostRequest[1] -ne $guestResult[1]) { Write-Host 'request ids differ between accepted request and its result'; $verdict = $false }
    if ($hostRequest[2] -ne $guestResult[2]) { Write-Host 'attacker ids differ'; $verdict = $false }
    if ($hostRequest[3] -ne $guestResult[3]) { Write-Host 'target ids differ'; $verdict = $false }
}

Write-Host ''
Write-Host ("VERDICT: {0}" -f ($(if ($verdict) { 'PASS' } else { 'FAIL' })))
if (-not $KeepOpen) {
    foreach ($p in @($hostProc, $guestProc)) {
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
}
if (-not $verdict) { exit 1 }

