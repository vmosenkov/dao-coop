<#
.SYNOPSIS
    Death + revive test: the host player falls, its world keeps running while
    the guest is alive, the fight is won and the host's party stands up again.

.DESCRIPTION
    Host: world -> ogre in sight -> handoff save -> fight; its party is NOT
    raised (DAOCOOP_DEMO_PARTY_HEALTH=0) so the player falls. A test hook
    (DAOCOOP_TEST_VICTORY=1) then defeats the ogre three seconds after the fall,
    which should trigger the revive path: fallen party members are set back to
    their maximum health and the host's control returns to its avatar.
    Guest: joins normally and keeps its own party alive.

    PASS requires, from the logs:
      - host: "test: the host player is down", "test: the hostile is defeated",
        "revive:" lines, the host's state still flowing after the fall, and the
        host's avatar back above zero health after the revive
      - guest: joined (game loaded slot 91)

    Windows are closed at the end.
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $AfterDeathSeconds = 120
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

Write-Host '--- host: world, ogre, handoff (party not raised, victory hook) ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '1'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '1'; $env:DAOCOOP_DEMO_PARTY_HEALTH = '0'
$env:DAOCOOP_DEMO_OGRE_HEALTH = '5000'; $env:DAOCOOP_TEST_VICTORY = '1'; $env:DAOCOOP_TEST_KILL_PLAYER = '1'
$env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Focus-GameWindow $hostProc 25
if (-not (Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds)) { throw 'host never reached the world' }

Write-Host '--- guest: join ---'
$env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
$env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = '1'
$env:DAOCOOP_DEMO_FIGHT = '1'; $env:DAOCOOP_DEMO_PARTY_HEALTH = '5000'
$env:DAOCOOP_DEMO_OGRE_HEALTH = '5000'; $env:DAOCOOP_TEST_VICTORY = '0'
$env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Focus-GameWindow $guestProc 25
$joined = Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds
Write-Host ("  guest joined: {0}" -f $joined)

Start-Sleep -Seconds 4
& (Join-Path $PSScriptRoot 'windowed.ps1') -Arrange -Width 960 -Height 540 2>&1 | ForEach-Object { Write-Host "  $_" }

$fight = Wait-ForLogMatch $hostProc (LogPath 'host') 'demo: fight started' $AfterDeathSeconds
Write-Host ("  host started the fight: {0}" -f $fight)
$fell = Wait-ForLogMatch $hostProc (LogPath 'host') 'test: the host player is down' $AfterDeathSeconds
Write-Host ("  host player fell: {0}" -f $fell)
$defeated = Wait-ForLogMatch $hostProc (LogPath 'host') 'test: the hostile is defeated' $AfterDeathSeconds
Write-Host ("  hostile defeated (test hook): {0}" -f $defeated)
$revived = Wait-ForLogMatch $hostProc (LogPath 'host') 'revive:' $AfterDeathSeconds
Write-Host ("  revive path ran: {0}" -f $revived)
Start-Sleep -Seconds 6

Write-Host ''
Write-Host '=== results ==='
$verdict = $true
$hostLog = LogPath 'host'
$guestLog = LogPath 'guest'
$hostAlive = $false
$hostRevived = $false
$ogreDead = $false

if (-not (Test-Path $hostLog)) { Write-Host 'no host log'; $verdict = $false }
else {
    $hostLines = Get-Content $hostLog
    $avatarId = $null
    foreach ($line in $hostLines) {
        if ($line -match "avatar: this player's creature is (0x[0-9a-f]+)") { $avatarId = $Matches[1] }
    }
    Write-Host ("host avatar: {0}" -f $avatarId)
    if (-not $avatarId) { $verdict = $false }

    $deathIndex = -1
    if ($avatarId) {
        $needle = $avatarId.Substring(2)
        for ($i = 0; $i -lt $hostLines.Count; $i++) {
            if ($hostLines[$i] -match ("0x" + $needle + ":.*health 0\.0")) { $deathIndex = $i; break }
        }
    }
    if ($deathIndex -lt 0) { Write-Host 'host player never logged at zero health'; $verdict = $false }
    else {
        $afterDeath = @($hostLines[($deathIndex + 1)..($hostLines.Count - 1)])
        $hostNetAfterDeath = @($afterDeath | Where-Object { $_ -match 'net role=host' })
        $hostAlive = $hostNetAfterDeath.Count -gt 5
        Write-Host ("host world kept running after the fall: {0} ({1} state lines)" -f $hostAlive, $hostNetAfterDeath.Count)
        if (-not $hostAlive) { $verdict = $false }

        $ogreDead = @($afterDeath | Where-Object { $_ -match 'test: the hostile is defeated' }).Count -gt 0
        Write-Host ("hostile defeated: {0}" -f $ogreDead)
        if (-not $ogreDead) { $verdict = $false }

        if ($avatarId) {
            $needle = $avatarId.Substring(2)
            $reviveLines = @($afterDeath | Where-Object { $_ -match 'revive: 0x' })
            $hostRevived = @($afterDeath | Where-Object {
                    $_ -match ("0x" + $needle + ":.*health ([1-9][0-9]*\.[0-9])")
                }).Count -gt 0
            Write-Host ("revive lines: {0}; host avatar above zero after revive: {1}" -f $reviveLines.Count, $hostRevived)
            if ($reviveLines.Count -lt 1) { $verdict = $false }
            if (-not $hostRevived) { $verdict = $false }
        }
    }
}

if (-not (Test-Path $guestLog)) { Write-Host 'no guest log'; $verdict = $false }
else {
    $guestJoined = @(Get-Content $guestLog | Where-Object { $_ -match 'game loaded: slot 91' }).Count -gt 0
    Write-Host ("guest joined: {0}" -f $guestJoined)
    if (-not $guestJoined) { $verdict = $false }
}

Write-Host ''
Write-Host ("VERDICT: {0}" -f ($(if ($verdict) { 'PASS' } else { 'FAIL' })))
foreach ($p in @($hostProc, $guestProc)) {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}
if (-not $verdict) { exit 1 }



