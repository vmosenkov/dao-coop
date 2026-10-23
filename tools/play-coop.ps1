<#
.SYNOPSIS
    Start a real two-window co-op session in E:\DAO-Coop-Test: the host loads
    the newest save and serves the handoff slot; the guest joins and takes the
    second party member. Nothing moves on its own (no test drive).

.DESCRIPTION
    Use this instead of starting the game twice by hand: both games would then
    read the same daocoop.ini (a test host config) and the guest would never
    join. Here every setting is passed as an environment variable, which wins
    over the ini.

    Both windows are arranged 960x540 side by side. Leave them running and
    play; stop them with .\tools\play-coop.ps1 -StopAll.

.PARAMETER StopAll
    Close any running daorigins processes and exit.
#>
[CmdletBinding()]
param(
    [switch] $StopAll,
    [int] $HostWaitSeconds = 180,
    [int] $GuestWaitSeconds = 240,
    [int] $Width = 960,
    [int] $Height = 540
)

$ErrorActionPreference = 'Stop'
$bin = 'E:\DAO-Coop-Test\bin_ship'
$exe = Join-Path $bin 'daorigins.exe'
$instanceRoot = Join-Path $env:LOCALAPPDATA 'DAOLib\instances'

if ($StopAll) {
    Get-Process daorigins -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Write-Host 'daorigins stopped.'
    return
}
if (-not (Test-Path $exe)) { throw "test install not found: $exe" }
if (Get-Process daorigins -ErrorAction SilentlyContinue) {
    throw 'daorigins is already running; stop it first (.\tools\play-coop.ps1 -StopAll)'
}

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

Write-Host '--- host: newest save, handoff on, walking only by your input ---'
$env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'
$env:DAOCOOP_AUTOLOAD = '1'; $env:DAOCOOP_HANDOFF = '1'; $env:DAOCOOP_JOIN = '0'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = ''
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
if (-not (Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds)) {
    throw 'host never reached the world; check the log under %LOCALAPPDATA%\DAOLib\instances\host'
}
Write-Host '  host is in the world'

Write-Host '--- guest: join the host''s world, take another party member ---'
$env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'
$env:DAOCOOP_AUTOLOAD = '0'; $env:DAOCOOP_HANDOFF = '0'; $env:DAOCOOP_JOIN = '1'
$env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = '1'
$env:DAOCOOP_DEMO_FIGHT = '0'; $env:DAOCOOP_COMBAT_TEST = '0'; $env:DAOCOOP_DRIVE = '0'
$guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
if (-not (Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds)) {
    throw 'guest never joined; check the log under %LOCALAPPDATA%\DAOLib\instances\guest'
}
Write-Host '  guest joined'

& (Join-Path $PSScriptRoot 'windowed.ps1') -Arrange -Width $Width -Height $Height 2>&1 | ForEach-Object { Write-Host "  $_" }
Write-Host ''
Write-Host 'Play: both windows are yours. The host is the world authority; the guest'
Write-Host 'takes the companion. Close the games with .\tools\play-coop.ps1 -StopAll.'
