<#
.SYNOPSIS
    Mirror-avatar animation test: both players walk, and each game must move
    the other player's creature with real move orders (so its own movement
    controller plays the run animation) instead of teleporting it.

.DESCRIPTION
    Host starts its world and serves the handoff save; the guest joins and
    selects its own party member. Both run the plugin's drive orders, which walk
    their controlled creature around a circle. The host must then log puppet
    move orders ("puppet_walks") and keep a small puppet distance; the same for
    the guest. The script prints the net lines, takes a PrintWindow screenshot
    of both windows, and gives a verdict.
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $WalkSeconds = 90,
    [int] $MinWalks = 10,
    [double] $MaxPuppetDistance = 2.0
)

$ErrorActionPreference = 'Stop'
$bin = 'E:\DAO-Coop-Test\bin_ship'
$exe = Join-Path $bin 'daorigins.exe'
$instanceRoot = Join-Path $env:LOCALAPPDATA 'DAOLib\instances'

if (-not (Test-Path $exe)) { throw "test install not found: $exe" }
if (Get-Process -Name 'daorigins' -ErrorAction SilentlyContinue) {
    throw 'daorigins already running; stop it first (.\tools\launch.ps1 -StopAll)'
}

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Win32 -Name WalkShot -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint nFlags);
public struct RECT { public int Left, Top, Right, Bottom; }
'@ -ErrorAction SilentlyContinue

function LogPath([string] $instance) {
    Join-Path $instanceRoot "$instance\BioWare\Dragon Age\daolib\daocoop.log"
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

function Save-WindowShot($process, [string] $path) {
    $process.Refresh()
    $handle = $process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { Write-Host '  no window'; return }
    [void][Win32.WalkShot]::ShowWindow($handle, 9)
    [void][Win32.WalkShot]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 500
    $rect = New-Object Win32.WalkShot+RECT
    [void][Win32.WalkShot]::GetWindowRect($handle, [ref]$rect)
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    $printed = [Win32.WalkShot]::PrintWindow($handle, $hdc, 2)
    $graphics.ReleaseHdc($hdc)
    $graphics.Dispose()
    if (-not $printed) {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $width, $height))
        $graphics.Dispose()
    }
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    Write-Host ("  screenshot: {0}" -f $path)
}

Write-Host '--- host: world + handoff, driving its creature ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '1'
$hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Start-Sleep -Seconds 20
if (-not (Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds)) { throw 'host never reached the world' }

Write-Host '--- guest: join, select party 1, driving its creature ---'
$env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
$env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = '1'
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '1'
$guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
$joined = Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds
Write-Host ("  guest joined: {0}" -f $joined)
& (Join-Path $PSScriptRoot 'windowed.ps1') -Arrange -Width 960 -Height 540 2>&1 | ForEach-Object { Write-Host "  $_" }

Write-Host ("--- walking for {0} s ---" -f $WalkSeconds)
Start-Sleep -Seconds $WalkSeconds

$verdict = $true
$hostNet = ''
$guestNet = ''
foreach ($name in 'host', 'guest') {
    $log = LogPath $name
    if (-not (Test-Path $log)) { Write-Host "  no $name log"; $verdict = $false; continue }
    $net = Get-Content $log | Where-Object { $_ -match 'net role=' } | Select-Object -Last 1
    $drives = (Get-Content $log | Where-Object { $_ -match 'drive: move ordered' }).Count
    if ($name -eq 'host') { $hostNet = $net } else { $guestNet = $net }
    Write-Host ("--- {0} ---" -f $name)
    Write-Host ("  drive orders: {0}" -f $drives)
    Write-Host ("  last net: {0}" -f $net)
    if ($drives -lt 10) { $verdict = $false }
    if ($net -match 'puppet_walks ([0-9]+)') {
        $walks = [int]$Matches[1]
        Write-Host ("  puppet walks: {0}" -f $walks)
        if ($walks -lt $MinWalks) { $verdict = $false }
    }
    else {
        Write-Host '  no puppet_walks count'
        $verdict = $false
    }
    if ($net -match 'puppet_distance ([0-9.]+)') {
        $distance = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
        if ($distance -gt $MaxPuppetDistance) { $verdict = $false }
    }
}

Save-WindowShot $hostProc 'E:\Temp_files\walk-host.png'
Save-WindowShot $guestProc 'E:\Temp_files\walk-guest.png'

Get-Process daorigins -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

if ($verdict) {
    Write-Host 'VERDICT: PASS - both games moved the other player''s creature with real move orders'
} else {
    Write-Host 'VERDICT: FAIL'
}
