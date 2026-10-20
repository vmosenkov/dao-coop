<#
.SYNOPSIS
    Start or stop one isolated Dragon Age: Origins instance from the test copy.

.DESCRIPTION
    A DAOLib named instance (DAOLIB_INSTANCE=<name>) gets its own process
    mutex and its own Documents profile under
    %LOCALAPPDATA%\DAOLib\instances\<name>, so two copies can run side by side
    without touching the owner's real profile. The test install is always
    E:\DAO-Coop-Test\x (never the real game).

.EXAMPLE
    .\tools\launch.ps1 -Instance host
    .\tools\launch.ps1 -Instance guest -Seconds 40    # start, wait, stop
    .\tools\launch.ps1 -StopAll
#>
[CmdletBinding()]
param(
    [ValidateSet('host', 'guest', 'solo')]
    [string] $Instance = 'host',
    [int] $Seconds = 0,
    [switch] $StopAll
)

$ErrorActionPreference = 'Stop'
$bin = 'E:\DAO-Coop-Test\bin_ship'
$exe = Join-Path $bin 'daorigins.exe'
$instanceRoot = Join-Path $env:LOCALAPPDATA 'DAOLib\instances'

if ($StopAll) {
    $procs = Get-Process -Name 'daorigins' -ErrorAction SilentlyContinue
    if (-not $procs) { Write-Host 'no daorigins process running'; return }
    $procs | Stop-Process -Force
    Write-Host "stopped $($procs.Count) daorigins process(es)"
    return
}

if (-not (Test-Path $exe)) { throw "test install not found: $exe" }
if ($Instance -ne 'solo') { $env:DAOLIB_INSTANCE = $Instance } else { Remove-Item Env:\DAOLIB_INSTANCE -ErrorAction SilentlyContinue }

$p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
Write-Host "started $Instance (PID $($p.Id))"
$log = if ($Instance -ne 'solo') { Join-Path $instanceRoot "$Instance\BioWare\Dragon Age\daolib\loader.log" } else { $null }
if ($log) { Write-Host "loader log: $log" }

if ($Seconds -gt 0) {
    Start-Sleep -Seconds $Seconds
    $alive = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    Write-Host ("after {0}s: alive {1}" -f $Seconds, [bool]$alive)
    if ($log -and (Test-Path $log)) { Get-Content $log -Tail 10 | ForEach-Object { Write-Host "  $_" } }
    if ($alive) { Stop-Process -Id $p.Id -Force }
}
