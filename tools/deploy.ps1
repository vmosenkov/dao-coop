<#
.SYNOPSIS
    Copy a dao-coop build into an ISOLATED TEST INSTALL of Dragon Age: Origins.

.DESCRIPTION
    Never point this at the owner's real game. The coop-anything rules say the
    owner's install and saves are not touched; the test copy is
    E:\DAO-Coop-Test (a full copy of the install). The script refuses any other
    folder unless -AllowAnyPath is given, and refuses while the game runs.

    It copies:
      <build>\dinput8.dll (+ .pdb)            the DAOLib loader
      <build>\daolib\addresses\*.bin          the address library's files
      <build>\daolib\plugins\daocoop\*        the plugin
    Logs afterwards are in Documents\BioWare\Dragon Age\daolib\
    (loader.log, daocoop.log).
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string] $Preset = 'debug',
    [string] $Game = 'E:\DAO-Coop-Test',
    [switch] $Remove,
    [switch] $AllowAnyPath
)

$ErrorActionPreference = 'Stop'
$build = "E:\dao-coop-build\$Preset"
$expected = 'E:\DAO-Coop-Test'

if (-not $AllowAnyPath -and ($Game.TrimEnd('\') -ne $expected)) {
    throw "Refusing to write to '$Game': this project only deploys into the isolated test install '$expected'."
}
if ((Get-Process -Name 'daorigins', 'DAOrigins' -ErrorAction SilentlyContinue)) {
    throw 'Dragon Age: Origins is running. Close it first (the loader DLL is held open).'
}
if (-not (Test-Path $Game)) {
    throw "Test install not found: $Game (copy the game there first)."
}

$pluginDir = Join-Path $Game 'bin_ship\daolib\plugins\daocoop'
if ($Remove) {
    if (Test-Path $pluginDir) { Remove-Item -Recurse -Force $pluginDir; Write-Host "removed $pluginDir" }
    return
}

if (-not (Test-Path (Join-Path $build 'dinput8.dll'))) {
    throw "No build at $build. Run tools\build.ps1 -Preset $Preset first."
}

New-Item -ItemType Directory -Force (Join-Path $Game 'bin_ship\daolib\addresses') | Out-Null
New-Item -ItemType Directory -Force $pluginDir | Out-Null

Copy-Item (Join-Path $build 'dinput8.dll') (Join-Path $Game 'bin_ship') -Force
if (Test-Path (Join-Path $build 'dinput8.pdb')) {
    Copy-Item (Join-Path $build 'dinput8.pdb') (Join-Path $Game 'bin_ship') -Force
}
$addresses = Join-Path $build 'daolib\addresses'
if (Test-Path $addresses) {
    Copy-Item (Join-Path $addresses '*.bin') (Join-Path $Game 'bin_ship\daolib\addresses') -Force
}
Copy-Item (Join-Path $build 'daolib\plugins\daocoop\*') $pluginDir -Force

Write-Host "Deployed $Preset build into $Game"
Write-Host "Plugin: $pluginDir"
Write-Host "Then start $Game\bin_ship\daorigins.exe and read Documents\BioWare\Dragon Age\daolib\daocoop.log"
