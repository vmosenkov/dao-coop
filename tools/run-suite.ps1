<#
.SYNOPSIS
    Run the automated tests one by one, each in its own PowerShell process.

.DESCRIPTION
    The tests use Add-Type to declare the same Win32 type names; two of them in
    one PowerShell session would collide. This runner starts a fresh pwsh for
    each test, stops the game between runs, saves each test's output under
    E:\Temp_files\dao-coop-suite and prints a summary. It exits non-zero if any
    test does not print VERDICT: PASS.
#>
[CmdletBinding()]
param(
    [string[]] $Tests = @('sync-test.ps1', 'join-test.ps1', 'two-avatar-test.ps1',
                         'combat-test.ps1', 'attack-test.ps1', 'death-test.ps1',
                         'reload-test.ps1', 'pause-test.ps1', 'walk-test.ps1'),
    [string] $OutDir = 'E:\Temp_files\dao-coop-suite'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force $OutDir | Out-Null

$results = @()
foreach ($test in $Tests) {
    $path = Join-Path $PSScriptRoot $test
    if (-not (Test-Path $path)) {
        $results += [pscustomobject]@{ Test = $test; Verdict = 'MISSING'; Seconds = 0 }
        continue
    }
    Get-Process daorigins -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
    $stem = [IO.Path]::GetFileNameWithoutExtension($test)
    $log = Join-Path $OutDir "$stem.log"
    $err = Join-Path $OutDir "$stem.err"
    $started = Get-Date
    Write-Host ("=== {0} ===" -f $test)
    $proc = Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $path
    ) -WorkingDirectory $repo -PassThru -RedirectStandardOutput $log -RedirectStandardError $err
    $proc.WaitForExit()
    $seconds = [math]::Round(((Get-Date) - $started).TotalSeconds)
    $text = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    $verdict = 'NO VERDICT'
    if ($text -match 'VERDICT: (PASS|FAIL)') { $verdict = $Matches[1] }
    Write-Host ("  {0} in {1}s (exit {2})" -f $verdict, $seconds, $proc.ExitCode)
    if ($verdict -ne 'PASS') {
        $text -split "`n" | Select-Object -Last 15 | ForEach-Object { Write-Host ("    " + $_.TrimEnd()) }
        if (Test-Path $err) {
            Get-Content $err | Select-Object -Last 6 | ForEach-Object { Write-Host ("    ! " + $_.TrimEnd()) }
        }
    }
    $results += [pscustomobject]@{ Test = $test; Verdict = $verdict; Seconds = $seconds }
}
Get-Process daorigins -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host '=== SUMMARY ==='
foreach ($r in $results) { Write-Host ("  {0,-22} {1,-8} {2,5}s" -f $r.Test, $r.Verdict, $r.Seconds) }
$failed = @($results | Where-Object { $_.Verdict -ne 'PASS' })
if ($failed.Count -gt 0) {
    Write-Host ''
    Write-Host ("SUITE: FAIL ({0} of {1})" -f $failed.Count, $results.Count)
    exit 1
}
Write-Host ''
Write-Host ("SUITE: PASS ({0} tests)" -f $results.Count)
