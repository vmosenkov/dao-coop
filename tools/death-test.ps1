<#
.SYNOPSIS
    Death handshake test: does the host's game keep running when its player
    falls while the guest is still alive, and does the host come back after the
    fight is won?

.DESCRIPTION
    Same rig as demo-fight.ps1, but the host's party is NOT raised
    (DAOCOOP_DEMO_PARTY_HEALTH=0) while the guest's is (5000). The ogre kills
    the host's Warden; the guest's dog keeps attacking it through the attack
    arbitration (the host executes the blows). Then the test checks:

      - the host's world kept running after its player fell (state packets go on)
      - the guest kept playing and sending attack requests
      - the ogre died
      - the host's player came back after the fight (health > 0 again)

    VERDICT: PASS only if all four hold. Windows are closed unless -KeepOpen.
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $FightWaitSeconds = 120,
    [int] $AfterDeathSeconds = 120,
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

Write-Host '--- host: world, ogre, handoff (party NOT raised) ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '1'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '1'; $env:DAOCOOP_DEMO_PARTY_HEALTH = '0'; $env:DAOCOOP_DEMO_OGRE_HEALTH = '300'
$env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Focus-GameWindow $hostProc 25
if (-not (Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds)) { throw 'host never reached the world' }

Write-Host '--- guest: join (party raised, so the fight can be won) ---'
$env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
$env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = '1'
$env:DAOCOOP_DEMO_FIGHT = '1'; $env:DAOCOOP_DEMO_PARTY_HEALTH = '5000'; $env:DAOCOOP_DEMO_OGRE_HEALTH = '300'
$env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Focus-GameWindow $guestProc 25
$joined = Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds
Write-Host ("  guest joined: {0}" -f ($null -ne $joined))

Start-Sleep -Seconds 4
& (Join-Path $PSScriptRoot 'windowed.ps1') -Arrange -Width 960 -Height 540 2>&1 | ForEach-Object { Write-Host "  $_" }

$fight = Wait-ForLogMatch $hostProc (LogPath 'host') 'demo: fight started' $FightWaitSeconds
Write-Host ("  host started the fight: {0}" -f ($null -ne $fight))

Write-Host '--- waiting for the host player to fall ---'
$death = Wait-ForLogMatch $hostProc (LogPath 'host') 'controlled 0x00000082:[^\r\n]*health 0\.0' $FightWaitSeconds
Write-Host ("  host player fell: {0}" -f ($null -ne $death))

Write-Host ("--- watching {0} s for the guest to win and the host to come back ---" -f $AfterDeathSeconds)
Start-Sleep -Seconds $AfterDeathSeconds

Write-Host ''
Write-Host '=== results ==='
$verdict = $true
$hostLog = LogPath 'host'
$guestLog = LogPath 'guest'
$hostAlive = $false
$guestKeptPlaying = $false
$ogreDead = $false
$revived = $false

if (-not (Test-Path $hostLog)) { Write-Host 'no host log'; $verdict = $false }
else {
    $hostLines = Get-Content $hostLog
    $deathIndex = -1
    for ($i = 0; $i -lt $hostLines.Count; $i++) {
        if ($hostLines[$i] -match 'controlled 0x00000082:.*health 0\.0') { $deathIndex = $i }
    }
    $hostNetAfterDeath = @($hostLines[($deathIndex + 1)..($hostLines.Count - 1)] | Where-Object { $_ -match 'net role=host' })
    $hostAlive = $hostNetAfterDeath.Count -gt 5
    Write-Host ("host world kept running after the fall: {0} ({1} state lines after it)" -f $hostAlive, $hostNetAfterDeath.Count)

    $afterDeath = @($hostLines[($deathIndex + 1)..($hostLines.Count - 1)])
    $ogreDead = @($afterDeath | Where-Object { $_ -match 'local_hostile_health 0\.0|local_hostile=0xffffffff' }).Count -gt 0
    $revived = @($afterDeath | Where-Object { $_ -match 'controlled 0x00000082:.*health ([1-9][0-9]*\.[0-9]+)' }).Count -gt 0
    Write-Host ("ogre dead: {0}" -f $ogreDead)
    Write-Host ("host player came back after the fight: {0}" -f $revived)
    if (-not $hostAlive) { $verdict = $false }
    if (-not $ogreDead) { $verdict = $false }
    if (-not $revived) { $verdict = $false }
}

if (-not (Test-Path $guestLog)) { Write-Host 'no guest log'; $verdict = $false }
else {
    $guestLines = Get-Content $guestLog
    $guestRequests = @($guestLines | Where-Object { $_ -match 'attack: request \d+ sent' }).Count
    $guestAccepted = @($guestLines | Where-Object { $_ -match 'attack: result for request \d+.*: accepted' }).Count
    $guestKeptPlaying = $guestRequests -gt 0
    Write-Host ("guest kept attacking: {0} request(s), {1} accepted" -f $guestRequests, $guestAccepted)
    if (-not $guestKeptPlaying) { $verdict = $false }
}

Write-Host ''
Write-Host ("VERDICT: {0}" -f ($(if ($verdict) { 'PASS' } else { 'FAIL' })))
if (-not $KeepOpen) {
    foreach ($p in @($hostProc, $guestProc)) {
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
}
if (-not $verdict) { exit 1 }

