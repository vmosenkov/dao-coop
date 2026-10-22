<#
.SYNOPSIS
    Guest rejoin test: the host loads another save during a session and the
    guest automatically reconnects to the new world.

.DESCRIPTION
    Host: world -> handoff save -> (test hook) load manual slot 2 -> save and
    serve the new world again.
    Guest: join, then when the host publishes kWorldReloaded it forgets the old
    join and receives/loads the new handoff save (the game shows its loading
    screen).

    PASS requires, from the logs:
      - host: "test: loading another save (slot 2)", "game loaded: slot 2",
        a second "handoff: sent the slot folder"
      - guest: "event: [world] Host loaded another save; reconnecting",
        a second "join: received the host's slot", a second
        "game loaded: slot 91", "Rejoined the host's new world"

    Windows are closed at the end.
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $RejoinWaitSeconds = 240
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

function Count-LogMatch([string] $log, [string] $pattern) {
    if (-not (Test-Path $log)) { return 0 }
    return @(Get-Content $log | Where-Object { $_ -match $pattern }).Count
}

function Wait-ForLogCount([string] $log, [string] $pattern, [int] $wanted, [int] $limitSeconds) {
    $deadline = (Get-Date).AddSeconds($limitSeconds)
    while ((Get-Date) -lt $deadline) {
        if ((Count-LogMatch $log $pattern) -ge $wanted) { return $true }
        Start-Sleep -Seconds 2
    }
    return $false
}

$hostProc = $null
$guestProc = $null
try {
    Write-Host '--- host: world -> save -> serve ---'
    $env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
    $env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
    $env:DAOCOOP_DRIVE = '0'; $env:DAOCOOP_ADD_COMPANION = '1'; $env:DAOCOOP_SELECT_PARTY = ''
    $env:DAOCOOP_TEST_RELOAD = '1'
    $hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Focus-GameWindow $hostProc 25
    if (-not (Wait-ForLogCount (LogPath 'host') 'game loaded:' 1 $HostWaitSeconds)) { throw 'host never reached the world' }

    Write-Host '--- guest: join ---'
    $env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
    $env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
    $env:DAOCOOP_DRIVE = '0'; $env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = '1'
    $env:DAOCOOP_TEST_RELOAD = '0'
    $guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Focus-GameWindow $guestProc 25
    if (-not (Wait-ForLogCount (LogPath 'guest') 'game loaded: slot 91' 1 $GuestWaitSeconds)) { throw 'guest never joined' }
    Write-Host '  guest joined; host will load another save'
    Start-Sleep -Seconds 4
    & (Join-Path $PSScriptRoot 'windowed.ps1') -Arrange -Width 960 -Height 540 2>&1 | ForEach-Object { Write-Host "  $_" }
}
finally {
    # keep running until the checks below time out or pass; killed at the end
}

Write-Host '--- waiting for the host reload and the guest rejoin ---'
$hostReload = Wait-ForLogCount (LogPath 'host') 'test: loading another save' 1 $RejoinWaitSeconds
Write-Host ("  host started the reload: {0}" -f $hostReload)
$hostNewWorld = Wait-ForLogCount (LogPath 'host') 'game loaded: slot 2' 1 $RejoinWaitSeconds
Write-Host ("  host loaded the new world: {0}" -f $hostNewWorld)
$hostServedAgain = Wait-ForLogCount (LogPath 'host') 'handoff: sent the slot folder' 2 $RejoinWaitSeconds
Write-Host ("  host served the new world: {0}" -f $hostServedAgain)
$guestNoticed = Wait-ForLogCount (LogPath 'guest') 'Host loaded another save; reconnecting' 1 $RejoinWaitSeconds
Write-Host ("  guest saw the reload: {0}" -f $guestNoticed)
$guestReceivedAgain = Wait-ForLogCount (LogPath 'guest') 'join: received the host' 2 $RejoinWaitSeconds
Write-Host ("  guest received the new save: {0}" -f $guestReceivedAgain)
$guestLoadedAgain = Wait-ForLogCount (LogPath 'guest') 'game loaded: slot 91' 2 $RejoinWaitSeconds
Write-Host ("  guest loaded the new save: {0}" -f $guestLoadedAgain)
$guestRejoined = Wait-ForLogCount (LogPath 'guest') 'Rejoined the host' 1 $RejoinWaitSeconds
Write-Host ("  guest rejoined: {0}" -f $guestRejoined)

$verdict = $hostReload -and $hostNewWorld -and $hostServedAgain -and $guestNoticed -and $guestReceivedAgain -and $guestLoadedAgain -and $guestRejoined
Write-Host ''
Write-Host ("VERDICT: {0}" -f ($(if ($verdict) { 'PASS' } else { 'FAIL' })))
foreach ($p in @($hostProc, $guestProc)) {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}
if (-not $verdict) { exit 1 }
