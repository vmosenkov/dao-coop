<#
.SYNOPSIS
    Bug check: the event log screen must not show in the main menu.

.DESCRIPTION
    Starts one instance with no autoload (it stays at the main menu), waits,
    screenshots the window and prints the UI log lines. Expected: the screen is
    added (registered) but never shown, so no panel is in the menu screenshot.
    Run tools\ui-shot.ps1 afterwards for the in-session case.
#>
[CmdletBinding()]
param(
    [int] $MenuWaitSeconds = 30,
    [switch] $Close
)

$ErrorActionPreference = 'Stop'
$bin = 'E:\DAO-Coop-Test\bin_ship'
$exe = Join-Path $bin 'daorigins.exe'
$instanceRoot = Join-Path $env:LOCALAPPDATA 'DAOLib\instances'
$shot = 'E:\Temp_files\ui-menu-shot.png'

if (-not (Test-Path $exe)) { throw "test install not found: $exe" }
if (Get-Process -Name 'daorigins' -ErrorAction SilentlyContinue) {
    throw 'daorigins already running; stop it first (.\tools\launch.ps1 -StopAll)'
}

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Win32 -Name MenuShot -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint nFlags);
public struct RECT { public int Left, Top, Right, Bottom; }
'@ -ErrorAction SilentlyContinue

Write-Host '--- host instance at the main menu (no autoload) ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$proc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru

$deadline = (Get-Date).AddSeconds(25)
while ((Get-Date) -lt $deadline) {
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
}
Start-Sleep -Seconds $MenuWaitSeconds
$proc.Refresh()
$handle = $proc.MainWindowHandle
if ($handle -eq [IntPtr]::Zero) { throw 'no game window' }
[void][Win32.MenuShot]::ShowWindow($handle, 9)
[void][Win32.MenuShot]::SetForegroundWindow($handle)
Start-Sleep -Milliseconds 700

$rect = New-Object Win32.MenuShot+RECT
[void][Win32.MenuShot]::GetWindowRect($handle, [ref]$rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$hdc = $graphics.GetHdc()
$printed = [Win32.MenuShot]::PrintWindow($handle, $hdc, 2)
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

$log = Join-Path $instanceRoot 'host\BioWare\Dragon Age\daolib\daocoop.log'
Write-Host ''
Write-Host '=== UI log lines ==='
$shown = $false
if (Test-Path $log) {
    $text = Get-Content $log -Raw
    $shown = $text -match 'ui: event log screen shown'
    Get-Content $log | Where-Object { $_ -match 'ui: ' } | Select-Object -First 10 | ForEach-Object { Write-Host ("  " + $_) }
}
Write-Host ''
if ($shown) {
    Write-Host 'VERDICT: FAIL - the panel was shown while no game session was running'
} else {
    Write-Host 'VERDICT: PASS - the screen was not shown in the main menu'
}

if ($Close) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Write-Host 'instance stopped'
} else {
    Write-Host 'window left open; close with .\tools\launch.ps1 -StopAll'
}
