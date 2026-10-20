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
    [int] $Jobs = 0,
    # Release without letting vcpkg touch the network: reuse the packages a
    # debug build already installed for the same x86 triplet.
    [switch] $Offline,
    [string] $InstalledDir = 'E:\dao-coop-build\debug\vcpkg_installed'
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
    # This machine's C++ toolset lives off the default location.
    $candidates += 'E:\VS2022BuildTools'
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
# The vcvars environment points VCPKG_ROOT at VS's bundled vcpkg; use ours.
$env:VCPKG_ROOT = 'E:\vcpkg'
if (-not (Test-Path (Join-Path $env:VCPKG_ROOT 'vcpkg.exe'))) {
    throw "vcpkg not bootstrapped at $env:VCPKG_ROOT (run bootstrap-vcpkg.bat there)."
}

# vcvarsall does not always put VS's CMake and Ninja on PATH; make sure both
# are reachable and pin Ninja for the preset ($env{NINJA_EXE}).
$vsRoot = (Resolve-Path (Join-Path (Split-Path $vcvars -Parent) '..\..\..')).Path
$cmakeBin = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaBin = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:PATH = "$cmakeBin;$ninjaBin;$env:PATH"
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $ninja) { throw "ninja not found under $ninjaBin" }
$env:NINJA_EXE = $ninja.Source
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw "cmake not found under $cmakeBin" }
if ($Jobs -gt 0) { $env:CMAKE_BUILD_PARALLEL_LEVEL = "$Jobs" }

if ($Fresh -and (Test-Path (Join-Path $buildRoot $Preset))) {
    Write-Host "Removing $(Join-Path $buildRoot $Preset)"
    Remove-Item -Recurse -Force (Join-Path $buildRoot $Preset)
}

Push-Location $repo
$buildLog = Join-Path $buildRoot "build-$Preset.log"
New-Item -ItemType Directory -Force $buildRoot | Out-Null
if (Test-Path $buildLog) { Remove-Item $buildLog -Force }
function Show-BuildFindings([string] $log, [string] $what) {
    Write-Host "$what output kept at $log"
    $found = Select-String -Path $log -Pattern 'error C[0-9]{4}|error LNK|fatal error|FAILED:|ninja: build stopped|error:' -ErrorAction SilentlyContinue
    if ($found) {
        Write-Host '--- errors ---'
        $found | Select-Object -First 40 | ForEach-Object { Write-Host ("  L{0}: {1}" -f $_.LineNumber, $_.Line.Trim()) }
    }
    else {
        Write-Host '--- no error lines; last 40 lines ---'
        Get-Content $log -Tail 40 | ForEach-Object { Write-Host "  $_" }
    }
}
try {
    # Keep the (very long) compiler output on disk; print only findings.
    if ($Offline) {
        if (-not (Test-Path $InstalledDir)) {
            throw "-Offline needs the packages a debug build installed; $InstalledDir is not there (run -Preset debug first)."
        }
        # vcpkg refuses to switch a build tree from manifest to non-manifest
        # mode, so start that preset's tree over.
        if (Test-Path (Join-Path $buildRoot $Preset)) { Remove-Item -Recurse -Force (Join-Path $buildRoot $Preset) }
        & cmake --preset $Preset -DVCPKG_MANIFEST_MODE=OFF "-DVCPKG_INSTALLED_DIR=$InstalledDir" *> $buildLog
    }
    else {
        & cmake --preset $Preset *> $buildLog
    }
    if ($LASTEXITCODE -ne 0) { Show-BuildFindings $buildLog 'configure'; throw "cmake configure failed ($LASTEXITCODE)" }
    # DAOLib declares the loader target but leaves it out of ALL when it is a
    # subdirectory, so ask for both targets by name.
    & cmake --build --preset $Preset --target daocoop loader *>> $buildLog
    if ($LASTEXITCODE -ne 0) { Show-BuildFindings $buildLog 'build'; throw "cmake build failed ($LASTEXITCODE)" }
}
finally {
    Pop-Location
}

Write-Host ''
Write-Host "Built:"
Get-ChildItem (Join-Path $buildRoot $Preset) -File -Filter 'dinput8.*' -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $($_.FullName)" }
Get-ChildItem (Join-Path $buildRoot "$Preset\daolib\plugins\daocoop") -File -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $($_.FullName)" }
