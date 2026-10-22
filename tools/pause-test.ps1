<#
.SYNOPSIS
    Pause synchronisation test: the guest presses pause, the host follows.

.DESCRIPTION
    Host: world + handoff save; guest: join by save transfer. The guest has
    DAOCOOP_TEST_PAUSE=1, so a few seconds after joining it presses pause with
    the engine's own PauseMessage. The host is expected to follow through its
    own PauseMessage, and both games should log a paused frame.

    PASS requires, from the logs:
      - guest: "pause: test pressed pause on the guest"
      - host:  "pause: following the other player (paused)"
      - host:  a frame line with "paused true"

    Windows are closed at the end.
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $PauseWaitSeconds = 90
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

$hostProc = $null
$guestProc = $null
try {
    Write-Host '--- host: world -> save -> serve ---'
    $env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
    $env:DAOCOOP_DRIVE = '0'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
    $env:DAOCOOP_AUTOLOAD = '1'
    $hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Focus-GameWindow $hostProc 25
    if (-not (Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds)) { throw 'host never reached the world' }

    Write-Host '--- guest: join, then press pause ---'
    $env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
    $env:DAOCOOP_DRIVE = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
    $env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_TEST_PAUSE = '1'
    $guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Focus-GameWindow $guestProc 25
    if (-not (Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds)) { throw 'guest never joined' }
    Write-Host '  guest joined'

    $pressed = Wait-ForLogMatch $guestProc (LogPath 'guest') 'pause: test pressed pause on the guest' $PauseWaitSeconds
    Write-Host ("  guest pressed pause: {0}" -f $pressed)
    $followed = Wait-ForLogMatch $hostProc (LogPath 'host') 'pause: following the other player \(paused\)' $PauseWaitSeconds
    Write-Host ("  host followed the pause: {0}" -f $followed)
    # Give the next frame line time to be written with paused true.
    Start-Sleep -Seconds 4
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
$hostPausedFrame = $false
$guestPausedFrame = $false
if (Test-Path $hostLog) {
    $hostLines = Get-Content $hostLog
    $hostPausedFrame = @($hostLines | Where-Object { $_ -match 'paused true' }).Count -gt 0
    $hostFollow = @($hostLines | Where-Object { $_ -match 'pause: following the other player \(paused\)' }).Count -gt 0
    Write-Host ("host followed the pause: {0}; frame paused true: {1}" -f $hostFollow, $hostPausedFrame)
    if (-not $hostFollow) { $verdict = $false }
    if (-not $hostPausedFrame) { $verdict = $false }
}
else { Write-Host 'no host log'; $verdict = $false }
if (Test-Path $guestLog) {
    $guestLines = Get-Content $guestLog
    $guestPressed = @($guestLines | Where-Object { $_ -match 'pause: test pressed pause on the guest' }).Count -gt 0
    $guestPausedFrame = @($guestLines | Where-Object { $_ -match 'paused true' }).Count -gt 0
    Write-Host ("guest pressed the pause: {0}; frame paused true: {1}" -f $guestPressed, $guestPausedFrame)
    if (-not $guestPressed) { $verdict = $false }
    if (-not $guestPausedFrame) { $verdict = $false }
}
else { Write-Host 'no guest log'; $verdict = $false }

Write-Host ''
Write-Host ("VERDICT: {0}" -f ($(if ($verdict) { 'PASS' } else { 'FAIL' })))
if (-not $verdict) { exit 1 }
