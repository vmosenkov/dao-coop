<#
.SYNOPSIS
    Build dao-coop (the plugin and the DAOLib loader) from any shell.

.DESCRIPTION
    CMakePresets.json pins CMAKE_CXX_COMPILER=cl.exe, which only exists inside
    the Visual Studio developer environment. This script finds the VS C++
    toolset, imports the x86 environment (vcvarsall amd64_x86: a 64-bit
    compiler producing 32-bit code, the game's architecture), points CMake at
    vcpkg, then configures and builds.

    Output: E:\dao-coop-build\<preset>\daolib\plugins\daocoop\daocoop.dll and
    E:\dao-coop-build\<preset>\dinput8.dll (the loader). Nothing is copied into
    any game folder here; tools\deploy.ps1 does that with the game closed.
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string] $Preset = 'debug',
    [switch] $Fresh,
    [int] $Jobs = 0
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = "E:\dao-coop-build"

function Find-VcVarsAll {
    $candidates = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $candidates += & $vswhere -all -prerelease -products * -property installationPath
    }
    foreach ($root in ${env:ProgramFiles}, ${env:ProgramFiles(x86)}) {
        foreach ($ed in 'Enterprise', 'Professional', 'Community', 'BuildTools') {
            $candidates += (Join-Path $root "Microsoft Visual Studio\2022\$ed")
        }
    }
    foreach ($c in ($candidates | Where-Object { $_ } | Select-Object -Unique)) {
        $vcvars = Join-Path $c 'VC\Auxiliary\Build\vcvarsall.bat'
        if (Test-Path $vcvars) { return $vcvars }
    }
    return $null
}

$vcvars = Find-VcVarsAll
if (-not $vcvars) {
    throw 'No Visual Studio C++ toolset found (no vcvarsall.bat). Install the "Desktop development with C++" workload.'
}
Write-Host "Using $vcvars"

# Import the developer environment into this session.
$envLines = & cmd.exe /c "`"$vcvars`" amd64_x86 >nul 2>&1 && set"
foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2])
    }
}

if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = 'E:\vcpkg' }
if (-not (Test-Path (Join-Path $env:VCPKG_ROOT 'vcpkg.exe'))) {
    throw "vcpkg not bootstrapped at $env:VCPKG_ROOT (run bootstrap-vcpkg.bat there)."
}
if ($Jobs -gt 0) { $env:CMAKE_BUILD_PARALLEL_LEVEL = "$Jobs" }

if ($Fresh -and (Test-Path (Join-Path $buildRoot $Preset))) {
    Write-Host "Removing $(Join-Path $buildRoot $Preset)"
    Remove-Item -Recurse -Force (Join-Path $buildRoot $Preset)
}

Push-Location $repo
try {
    & cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
    & cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }
}
finally {
    Pop-Location
}

Write-Host ''
Write-Host "Built:"
Get-ChildItem (Join-Path $buildRoot $Preset) -File -Filter 'dinput8.*' -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $($_.FullName)" }
Get-ChildItem (Join-Path $buildRoot "$Preset\daolib\plugins\daocoop") -File -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $($_.FullName)" }
