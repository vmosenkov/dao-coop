<#
.SYNOPSIS
    Milestone-2 smoke test for the in-game event log: two instances, join, then
    a screenshot of the host window and the UI log lines.

.DESCRIPTION
    Host: world -> handoff save. Guest: joins by save transfer. After the join
    the event log should show real events from EventLog ("Another player joined
    the game", "Joined the host's game"), not only the sample lines. The script
    captures the host window to E:\Temp_files\ui-join-shot.png and leaves both
    windows open.
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $AfterJoinSeconds = 15,
    [switch] $Close
)

$ErrorActionPreference = 'Stop'
$bin = 'E:\DAO-Coop-Test\bin_ship'
$exe = Join-Path $bin 'daorigins.exe'
$instanceRoot = Join-Path $env:LOCALAPPDATA 'DAOLib\instances'
$shot = 'E:\Temp_files\ui-join-shot.png'

if (-not (Test-Path $exe)) { throw "test install not found: $exe" }
if (Get-Process -Name 'daorigins' -ErrorAction SilentlyContinue) {
    throw 'daorigins already running; stop it first (.\tools\launch.ps1 -StopAll)'
}

function LogPath([string] $instance) {
    Join-Path $instanceRoot "$instance\BioWare\Dragon Age\daolib\daocoop.log"
}

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Win32 -Name Ui -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint nFlags);
public struct RECT { public int Left, Top, Right, Bottom; }
'@ -ErrorAction SilentlyContinue

function Focus-GameWindow($process, [int] $seconds = 25) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $process.Refresh()
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
            [void][Win32.Ui]::ShowWindow($process.MainWindowHandle, 9)
            [void][Win32.Ui]::SetForegroundWindow($process.MainWindowHandle)
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

Write-Host '--- host ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Focus-GameWindow $hostProc 25
if (-not (Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds)) { throw 'host never reached the world' }

Write-Host '--- guest ---'
$env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
$env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = '1'
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Focus-GameWindow $guestProc 25
$joined = Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds
Write-Host ("  guest joined: {0}" -f $joined)
Start-Sleep -Seconds 4
& (Join-Path $PSScriptRoot 'windowed.ps1') -Arrange -Width 960 -Height 540 2>&1 | ForEach-Object { Write-Host "  $_" }

Write-Host ("--- waiting {0} s for the events ---" -f $AfterJoinSeconds)
Start-Sleep -Seconds $AfterJoinSeconds

$hostProc.Refresh()
$handle = $hostProc.MainWindowHandle
if ($handle -eq [IntPtr]::Zero) { throw 'no host game window' }
[void][Win32.Ui]::SetForegroundWindow($handle)
Start-Sleep -Milliseconds 500
$rect = New-Object Win32.Ui+RECT
[void][Win32.Ui]::GetWindowRect($handle, [ref]$rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$hdc = $graphics.GetHdc()
$printed = [Win32.Ui]::PrintWindow($handle, $hdc, 2)
$graphics.ReleaseHdc($hdc)
$graphics.Dispose()
if (-not $printed) {
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $width, $height))
    $graphics.Dispose()
}
$bitmap.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
Write-Host ("  screenshot: {0}" -f $shot)

Write-Host ''
Write-Host '=== UI log lines ==='
foreach ($name in 'host', 'guest') {
    Write-Host ("--- {0} ---" -f $name)
    $log = LogPath $name
    if (Test-Path $log) {
        Get-Content $log | Where-Object { $_ -match 'ui: |event: \[' } | Select-Object -First 12 | ForEach-Object { Write-Host ("  " + $_) }
    }
}

if ($Close) {
    foreach ($p in @($hostProc, $guestProc)) {
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
    Write-Host 'instances stopped'
} else {
    Write-Host 'windows left open; close with .\tools\launch.ps1 -StopAll'
}
