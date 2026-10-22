<#
.SYNOPSIS
    A VISIBLE fight: host and guest join, an ogre spawns a few metres in front
    of the party, its AI is turned back on, and both players' creatures are
    ordered to attack it. DAO resolves the fight itself.

.DESCRIPTION
    This is the demonstration counterpart of combat-test.ps1 (which damages a
    far-away, AI-off ogre through the plugin, so nothing moves on screen).

    The windows stay open so a person can watch; close them with
    .\tools\launch.ps1 -StopAll (or -KeepOpen:$false here to close at the end).

    Env: DAOCOOP_DEMO_FIGHT=1 on both instances; the host also runs the handoff
    join (save + serve), the guest joins by save transfer like a real session.
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $WatchSeconds = 120,
    [switch] $KeepOpen = $true
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

Write-Host '--- starting host (world -> save+serve -> fight) ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '1'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '1'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$env:DAOCOOP_DEMO_OGRE_HEALTH = '20000'
$hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Focus-GameWindow $hostProc 25
if (-not (Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds)) { throw 'host never reached the world' }
Write-Host '  host in the world'

Write-Host '--- starting guest (join) ---'
$env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
$env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = '1'
$env:DAOCOOP_DEMO_FIGHT = '1'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$env:DAOCOOP_DEMO_OGRE_HEALTH = '20000'
$guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Focus-GameWindow $guestProc 25
$joined = Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds
Write-Host ("  guest joined: {0}" -f $joined)

# Put the two windows side by side so the fight is visible on one screen.
Start-Sleep -Seconds 4
& (Join-Path $PSScriptRoot 'windowed.ps1') -Arrange -Width 960 -Height 540 2>&1 | ForEach-Object { Write-Host "  $_" }

$fight = Wait-ForLogMatch $hostProc (LogPath 'host') 'demo: fight started' 120
Write-Host ("  host started the fight: {0}" -f $fight)
Write-Host ("--- watching for {0} s: the party is attacking the ogre ---" -f $WatchSeconds)
Start-Sleep -Seconds $WatchSeconds

Write-Host ''
Write-Host '=== evidence from the logs ==='
foreach ($name in 'host', 'guest') {
    $log = LogPath $name
    Write-Host "--- $name ---"
    if (Test-Path $log) {
        Select-String -Path $log -Pattern 'demo:|hostile-table:|net role=' |
            Select-Object -Last 4 | ForEach-Object { Write-Host ("  " + $_.Line) }
    }
    else { Write-Host '  no log' }
}

if (-not $KeepOpen) {
    foreach ($p in @($hostProc, $guestProc)) {
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
    Write-Host 'instances stopped'
}
else {
    Write-Host ''
    Write-Host 'Windows left open: watch the fight. Close with .\tools\launch.ps1 -StopAll'
}

