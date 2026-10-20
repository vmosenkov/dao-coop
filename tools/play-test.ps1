<#
.SYNOPSIS
    Start host and guest instances together for a fixed time, then stop both
    and print their DAOLib logs. The two-client test rig, without a compiler.

.DESCRIPTION
    Instances: DAOLIB_INSTANCE=host and =guest from E:\DAO-Coop-Test.
    Logs:      %LOCALAPPDATA%\DAOLib\instances\{host,guest}\BioWare\Dragon Age\daolib\
    This never touches the owner's real profile.

.EXAMPLE
    .\tools\play-test.ps1 -Seconds 60
#>
[CmdletBinding()]
param(
    [int] $Seconds = 60,
    [string[]] $Logs = @('loader.log', 'daocoop.log')
)

$ErrorActionPreference = 'Stop'
$bin = 'E:\DAO-Coop-Test\bin_ship'
$exe = Join-Path $bin 'daorigins.exe'
$instanceRoot = Join-Path $env:LOCALAPPDATA 'DAOLib\instances'

if (-not (Test-Path $exe)) { throw "test install not found: $exe" }
if (Get-Process -Name 'daorigins' -ErrorAction SilentlyContinue) { throw 'daorigins already running; stop it first (.\tools\launch.ps1 -StopAll)' }

$started = @()
try {
    foreach ($name in 'host', 'guest') {
        $env:DAOLIB_INSTANCE = $name
        $p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
        $started += [pscustomobject]@{ Name = $name; Process = $p }
        Write-Host "started $name (PID $($p.Id))"
        Start-Sleep -Seconds 15
    }
    Write-Host "both started; running for $Seconds s"
    Start-Sleep -Seconds $Seconds
}
finally {
    foreach ($s in $started) {
        if ($s.Process -and -not $s.Process.HasExited) { Stop-Process -Id $s.Process.Id -Force -ErrorAction SilentlyContinue }
    }
    Start-Sleep -Seconds 3
}

foreach ($name in 'host', 'guest') {
    $dir = Join-Path $instanceRoot "$name\BioWare\Dragon Age\daolib"
    foreach ($f in $Logs) {
        $p = Join-Path $dir $f
        Write-Host ''
        Write-Host "=== $name / $f ==="
        if (Test-Path $p) { Get-Content $p -Tail 40 | ForEach-Object { Write-Host "  $_" } } else { Write-Host '  (missing)' }
    }
}
