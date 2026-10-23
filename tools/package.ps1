<#
.SYNOPSIS
    Package a release build as the zip a player installs.

.DESCRIPTION
    Builds the release preset (offline: reusing the packages a debug build
    installed) and lays the result out as the game folder plus install notes:

        bin_ship\dinput8.dll                  the DAOLib loader
        bin_ship\dinput8.pdb                  (crash logs name its code)
        bin_ship\daolib\plugins\daocoop\...   this mod
        bin_ship\daolib\addresses\*.bin       the address library
        daocoop.ini.example                   the settings file to copy
        README-INSTALL.txt                    what a player does

    Output: E:\dao-coop-build\dist\dao-coop-<version>.zip
#>
[CmdletBinding()]
param(
    [string] $Version = '0.7.5',
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = 'E:\dao-coop-build\release'
$dist = 'E:\dao-coop-build\dist'

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Preset release -Offline
    if ($LASTEXITCODE -ne 0) { throw 'release build failed' }
}

$needed = @(
    (Join-Path $build 'dinput8.dll'),
    (Join-Path $build 'daolib\plugins\daocoop\daocoop.dll')
)
foreach ($file in $needed) {
    if (-not (Test-Path $file)) { throw "missing release artifact: $file (build first)" }
}

New-Item -ItemType Directory -Force $dist | Out-Null
$stage = Join-Path $dist "stage-$Version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$ship = Join-Path $stage 'bin_ship'
$plugin = Join-Path $ship 'daolib\plugins\daocoop'
New-Item -ItemType Directory -Force $plugin | Out-Null
New-Item -ItemType Directory -Force (Join-Path $ship 'daolib\addresses') | Out-Null

Copy-Item (Join-Path $build 'dinput8.dll') $ship
Copy-Item (Join-Path $build 'dinput8.pdb') $ship -ErrorAction SilentlyContinue
Copy-Item (Join-Path $build 'daolib\plugins\daocoop\daocoop.dll') $plugin
Copy-Item (Join-Path $build 'daolib\plugins\daocoop\daocoop.pdb') $plugin -ErrorAction SilentlyContinue
Copy-Item (Join-Path $build 'daolib\plugins\daocoop\coop_eventlog.swf') $plugin -ErrorAction SilentlyContinue
Copy-Item (Join-Path $build 'daolib\addresses\*.bin') (Join-Path $ship 'daolib\addresses')
Copy-Item (Join-Path $repo 'daocoop.ini.example') $stage
Copy-Item (Join-Path $repo 'tools\README-INSTALL.txt') $stage
Copy-Item (Join-Path $repo 'tools\install.ps1') $stage

$zip = Join-Path $dist "dao-coop-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item -Recurse -Force $stage

Write-Host "packaged: $zip"
Get-Item $zip | ForEach-Object { Write-Host ("  {0:N1} MB" -f ($_.Length / 1MB)) }







