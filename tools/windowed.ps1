<#
.SYNOPSIS
    Put the test game instances into windowed mode, side by side, so both
    copies are visible on one screen.

.DESCRIPTION
    A DAOLib named instance has its own Documents profile under
    %LOCALAPPDATA%\DAOLib\instances\<name>; its display settings are in
    Settings\DragonAge.ini ([VideoOptions] FullScreen, ResolutionWidth,
    ResolutionHeight) and Settings\DAOriginsConfig.ini (WindowPosition).

    This script writes those files (the owner's real profile is never touched):
      - FullScreen = 0
      - the requested window size (default 960x540, two fit a 1920x1080 screen)
      - left/right positions from -Layout
    Run it before starting the instances; -Show reports what is set.

.EXAMPLE
    .\tools\windowed.ps1 -Show
    .\tools\windowed.ps1 -Instances host,guest -Width 960 -Height 540
#>
[CmdletBinding()]
param(
    [string[]] $Instances = @('host', 'guest'),
    [int] $Width = 960,
    [int] $Height = 540,
    [string[]] $Layout = @('left', 'right'),
    [switch] $Show,
    # Move the windows of already running copies side by side (the game sets
    # its own window position at start-up, so the ini value alone does not
    # place them). First started copy left, second right.
    [switch] $Arrange
)

$ErrorActionPreference = 'Stop'
$instanceRoot = Join-Path $env:LOCALAPPDATA 'DAOLib\instances'

function Get-InstanceSettings([string] $name) {
    $root = Join-Path $instanceRoot "$name\BioWare\Dragon Age\Settings"
    [pscustomobject]@{
        Name = $name
        Ini  = Join-Path $root 'DragonAge.ini'
        Cfg  = Join-Path $root 'DAOriginsConfig.ini'
    }
}

function Set-IniValue([string] $path, [string] $section, [string] $key, [string] $value) {
    $lines = @(Get-Content $path -ErrorAction SilentlyContinue)
    if ($lines.Count -eq 0) { throw "no such file: $path" }
    $sectionStart = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim() -eq "[$section]") { $sectionStart = $i; break }
    }
    if ($sectionStart -lt 0) { throw "section [$section] not found in $path" }
    $sectionEnd = $lines.Count
    for ($i = $sectionStart + 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim().StartsWith('[')) { $sectionEnd = $i; break }
    }
    $written = $false
    for ($i = $sectionStart + 1; $i -lt $sectionEnd; $i++) {
        if ($lines[$i] -match ("^\s*" + [regex]::Escape($key) + "\s*=")) {
            $lines[$i] = "$key=$value"; $written = $true; break
        }
    }
    if (-not $written) { $lines = $lines[0..$sectionEnd-1] + "$key=$value" + $lines[$sectionEnd..($lines.Count-1)] }
    Set-Content -Path $path -Value $lines -Encoding utf8
}

if ($Arrange) {
    Add-Type -Namespace Win32 -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hWnd, int X, int Y, int nWidth, int nHeight, bool repaint);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
public struct RECT { public int Left, Top, Right, Bottom; }
'@ -ErrorAction SilentlyContinue
    $procs = @(Get-Process daorigins -ErrorAction SilentlyContinue | Sort-Object StartTime)
    if ($procs.Count -eq 0) { throw 'no daorigins process is running; start the instances first' }
    for ($i = 0; $i -lt $procs.Count; $i++) {
        $p = $procs[$i]
        $p.Refresh()
        $h = $p.MainWindowHandle
        if ($h -eq [IntPtr]::Zero) { continue }
        $left = if (($i % 2) -eq 1) { $Width } else { 0 }
        [void][Win32.Win]::MoveWindow($h, $left, 0, $Width, $Height, $true)
        $rect = New-Object Win32.Win+RECT
        [void][Win32.Win]::GetWindowRect($h, [ref]$rect)
        Write-Host ("window {0}: PID {1} -> {2},{3} {4}x{5}" -f $i, $p.Id, $rect.Left, $rect.Top,
            ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top))
    }
    return
}

$index = 0
foreach ($name in $Instances) {
    $settings = Get-InstanceSettings $name
    if (-not (Test-Path $settings.Ini)) { throw "instance '$name' has no profile yet (start it once first): $($settings.Ini)" }
    if ($Show) {
        Write-Host "=== $name ==="
        Select-String -Path $settings.Ini -Pattern 'FullScreen|ResolutionWidth|ResolutionHeight|UseVSync|RefreshRate' |
            ForEach-Object { Write-Host ("  " + $_.Line) }
        if (Test-Path $settings.Cfg) { Get-Content $settings.Cfg | ForEach-Object { Write-Host ("  cfg: " + $_) } }
        continue
    }

    Set-IniValue $settings.Ini 'VideoOptions' 'FullScreen' '0'
    Set-IniValue $settings.Ini 'VideoOptions' 'ResolutionWidth' "$Width"
    Set-IniValue $settings.Ini 'VideoOptions' 'ResolutionHeight' "$Height"
    Set-IniValue $settings.Ini 'VideoOptions' 'UseVSync' '1'
    Set-IniValue $settings.Ini 'VideoOptions' 'RefreshRate' '60'

    # The launcher's window rectangle: left,top,right,bottom on the primary screen.
    $side = if ($index -lt $Layout.Count) { $Layout[$index] } else { 'left' }
    $left = if ($side -eq 'right') { $Width } else { 0 }
    $top = 0
    $rect = "$left,$top,$($left + $Width),$($top + $Height)"
    $cfgLines = @('[DAOriginsConfig]', "WindowPosition=$rect")
    Set-Content -Path $settings.Cfg -Value $cfgLines -Encoding utf8

    Write-Host ("{0}: windowed {1}x{2} at {3}" -f $name, $Width, $Height, $rect)
    $index++
}

if (-not $Show) {
    Write-Host ''
    Write-Host 'Run the game instances now; both windows should sit side by side.'
    Write-Host 'Restart the game after this change: the display settings are read at start-up.'
}
