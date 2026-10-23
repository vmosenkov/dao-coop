<#
.SYNOPSIS
    Smoke test for the in-game event log screen: start one instance, wait for
    the world, take a screenshot of the game window and read the UI log lines.

.DESCRIPTION
    The plugin adds ui/coop_eventlog.swf as a HUD screen (dao::gui::AddScreen)
    once the GUI system is up; the screen shows "Coop Event Log initialized."
    plus the last events (sample lines until the plugin pushes some).
    This script starts the host instance in the isolated test install, waits,
    captures the window to E:\Temp_files\ui-shot.png and prints the UI log
    lines. The window stays open for a human to look at it.
#>
[CmdletBinding()]
param(
    [int] $WorldWaitSeconds = 150,
    [int] $AfterWorldSeconds = 20,
    [switch] $Close
)

$ErrorActionPreference = 'Stop'
$bin = 'E:\DAO-Coop-Test\bin_ship'
$exe = Join-Path $bin 'daorigins.exe'
$instanceRoot = Join-Path $env:LOCALAPPDATA 'DAOLib\instances'
$shot = 'E:\Temp_files\ui-shot.png'

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

Write-Host '--- host: world + event log screen ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$proc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Focus-GameWindow $proc 25
$world = Wait-ForLogMatch $proc (LogPath 'host') 'game loaded:' $WorldWaitSeconds
Write-Host ("  host reached the world: {0}" -f $world)
if (-not $world) { throw 'host never reached the world' }

Start-Sleep -Seconds $AfterWorldSeconds
$proc.Refresh()
$handle = $proc.MainWindowHandle
if ($handle -eq [IntPtr]::Zero) { throw 'no game window' }
[void][Win32.Ui]::SetForegroundWindow($handle)
Start-Sleep -Milliseconds 500

$rect = New-Object Win32.Ui+RECT
[void][Win32.Ui]::GetWindowRect($handle, [ref]$rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
Write-Host ("  window {0}x{1} at {2},{3}" -f $width, $height, $rect.Left, $rect.Top)
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
$log = LogPath 'host'
if (Test-Path $log) {
    Get-Content $log | Where-Object { $_ -match 'ui: |event: \[' } | Select-Object -First 20 | ForEach-Object { Write-Host ("  " + $_) }
    if (-not (Get-Content $log | Where-Object { $_ -match 'ui: ' })) { Write-Host '  (no ui: lines: the screen was not added)' }
}

if ($Close) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Write-Host 'instance stopped'
} else {
    Write-Host 'window left open; close with .\tools\launch.ps1 -StopAll'
}

