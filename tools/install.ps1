<#
.SYNOPSIS
    Install the dao-coop release zip into a Dragon Age: Origins folder and write
    the player's daocoop.ini.

.DESCRIPTION
    Both players install the same release zip into their own game folder, then
    run this script once on each PC with that PC's role:

        .\install.ps1 -GameDir "C:\...\Dragon Age Ultimate Edition" -Role host -Apply
        .\install.ps1 -GameDir "C:\...\Dragon Age Ultimate Edition" -Role guest -PeerIp 192.168.1.50 -Apply

    By default it only prints what it would do; -Apply copies the files. It
    refuses a folder without daorigins.exe, backs up an existing daocoop.ini,
    and never deletes anything. Firewall ports are printed, not changed.

.PARAMETER GameDir
    The game folder (the one that holds bin_ship and daorigins.exe).

.PARAMETER Role
    host or guest.

.PARAMETER PeerIp
    host: the guest's LAN address (optional; the host can leave it).
    guest: the host's LAN address (required for a LAN game).

.PARAMETER Zip
    The release zip; by default the newest dao-coop-*.zip in E:\dao-coop-build\dist.

.PARAMETER Apply
    Copy the files. Without it the script only prints the plan.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $GameDir,
    [Parameter(Mandatory = $true)] [ValidateSet('host', 'guest')] [string] $Role,
    [string] $PeerIp = '',
    [string] $Zip = '',
    [switch] $Apply
)

$ErrorActionPreference = 'Stop'

if (-not $Zip) {
    $dist = 'E:\dao-coop-build\dist'
    $newest = Get-ChildItem $dist -Filter 'dao-coop-*.zip' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $newest) { throw "no release zip in $dist; build one with tools\package.ps1" }
    $Zip = $newest.FullName
}
if (-not (Test-Path $Zip)) { throw "release zip not found: $Zip" }

if (-not (Test-Path $GameDir)) { throw "game folder not found: $GameDir" }
$exe = Join-Path $GameDir 'daorigins.exe'
if (-not (Test-Path $exe)) {
    $exe = Join-Path $GameDir 'bin_ship\daorigins.exe'
}
if (-not (Test-Path $exe)) { throw "not a Dragon Age: Origins folder (no daorigins.exe): $GameDir" }

if ($Role -eq 'guest' -and -not $PeerIp) {
    throw 'a guest needs -PeerIp with the host''s LAN address'
}

$pluginDir = Join-Path $GameDir 'bin_ship\daolib\plugins\daocoop'
$iniPath = Join-Path $pluginDir 'daocoop.ini'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

Write-Host ("release: {0}" -f $Zip)
Write-Host ("game:    {0}" -f $GameDir)
Write-Host ("role:    {0}" -f $Role)
if ($PeerIp) { Write-Host ("peer:    {0}" -f $PeerIp) }
Write-Host ''

if (-not $Apply) {
    Write-Host 'Plan (nothing written):'
    Write-Host '  extract the zip over the game folder (bin_ship\dinput8.dll, bin_ship\daolib\addresses, bin_ship\daolib\plugins\daocoop)'
    Write-Host ("  write {0}" -f $iniPath)
    Write-Host 'Run again with -Apply to do it.'
    return
}

# Backup the ini before it is replaced.
if (Test-Path $iniPath) {
    $backup = "$iniPath.$stamp.bak"
    Copy-Item $iniPath $backup
    Write-Host ("  backed up the old daocoop.ini to {0}" -f $backup)
}

# Extract the zip over the game folder.
Expand-Archive -Path $Zip -DestinationPath $GameDir -Force
Write-Host '  extracted the release over the game folder'

# Write the player's configuration.
New-Item -ItemType Directory -Force $pluginDir | Out-Null
$handoff = if ($Role -eq 'host') { 1 } else { 0 }
$join = if ($Role -eq 'guest') { 1 } else { 0 }
$autoload = if ($Role -eq 'host') { 1 } else { 0 }
$companion = if ($Role -eq 'host') { 1 } else { 0 }
$selectParty = if ($Role -eq 'guest') { 1 } else { 0 }
$peer = if ($PeerIp) { $PeerIp } else { '127.0.0.1' }
$ini = @"
# dao-coop configuration written by tools\install.ps1 on $(Get-Date -Format 'yyyy-MM-dd HH:mm')
[net]
role = $Role
peer = $peer
host_port = 45830
guest_port = 45831
tcp_port = 45832

[session]
handoff = $handoff
join = $join
add_companion = $companion
select_party = $selectParty
autoload = $autoload

[test]
drive = 0
combat_test = 0
"@
Set-Content -Path $iniPath -Value $ini -Encoding ascii
Write-Host ("  wrote {0}" -f $iniPath)
Write-Host ''
Write-Host 'Firewall (open on the host PC): UDP 45830 in, TCP 45832 in; on the guest: UDP 45831 in.'
if ($Role -eq 'host') {
    Write-Host 'Start the game, load a save, play. The guest joins from the main menu.'
} else {
    Write-Host 'Start the game, wait at the main menu. The host''s world loads into your game.'
}
