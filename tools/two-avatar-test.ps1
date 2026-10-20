<#
.SYNOPSIS
    Milestone-4 test: two separate avatars — each player controls a different
    party member and each game mirrors the other's creature.

.DESCRIPTION
    Host: creates a mabari beside the player and adds it to the party through the
    engine's own AddToParty (command 396), then saves and serves the slot folder.
    Guest: joins by save transfer (M3), so it loads a world that already has the
    second party member; selects party index 1 (SelectPartyMemberIndexMessage)
    and drives that creature. The host keeps the Warden. Each side applies the
    other player's creature position every frame (SetPosition) and logs a
    "net role=..." line once a second.

    PASS requires:
      - host log: "companion: created", party list >= 2, handoff sent
      - guest log: "game loaded: slot 91", "select: asking for a click on index 1",
        a control change line
      - both last net lines: local id != remote id, puppet_applied >= 20,
        puppet_distance <= 2 m, same area id

    Deploy first: .\tools\deploy.ps1 -Preset debug
#>
[CmdletBinding()]
param(
    [int] $HostWaitSeconds = 150,
    [int] $GuestWaitSeconds = 180,
    [int] $SyncSeconds = 90,
    [double] $MaxDistance = 2.0,
    [int] $MinPuppetApplies = 20
)

$ErrorActionPreference = 'Stop'
$bin = 'E:\DAO-Coop-Test\bin_ship'
$exe = Join-Path $bin 'daorigins.exe'
$instanceRoot = Join-Path $env:LOCALAPPDATA 'DAOLib\instances'

if (-not (Test-Path $exe)) { throw "test install not found: $exe" }
if (Get-Process -Name 'daorigins' -ErrorAction SilentlyContinue) {
    throw 'daorigins already running; stop it first (.\tools\launch.ps1 -StopAll)'
}

function LogPath([string] $instance) {
    Join-Path $instanceRoot "$instance\BioWare\Dragon Age\daolib\daocoop.log"
}

function Focus-GameWindow($process, [int] $seconds = 25) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $process.Refresh()
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
            Add-Type -Namespace Win32 -Name User32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
'@ -ErrorAction SilentlyContinue
            [void][Win32.User32]::ShowWindow($process.MainWindowHandle, 9)
            [void][Win32.User32]::SetForegroundWindow($process.MainWindowHandle)
            return
        }
        Start-Sleep -Milliseconds 500
    }
}

function Wait-ForLogMatch($process, [string] $log, [string] $pattern, [int] $limitSeconds) {
    $deadline = (Get-Date).AddSeconds($limitSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($process.HasExited) { return $false }
        if (Test-Path $log) {
            if ((Get-Content $log -Raw -ErrorAction SilentlyContinue) -match $pattern) { return $true }
        }
        Start-Sleep -Seconds 2
    }
    return $false
}

$hostProc = $null
$guestProc = $null
try {
    Write-Host '--- starting host (world -> companion -> save+serve) ---'
    $env:DAOLIB_INSTANCE = 'host'; $env:DAOCOOP_ROLE = 'host'; $env:DAOCOOP_DRIVE = '1'; $env:DAOCOOP_HANDOFF = '1'
    $env:DAOCOOP_JOIN = '0'; $env:DAOCOOP_ADD_COMPANION = '1'; $env:DAOCOOP_SELECT_PARTY = ''
    $env:DAOCOOP_AUTOLOAD = '1'
    $hostProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Focus-GameWindow $hostProc 25
    $hostWorld = Wait-ForLogMatch $hostProc (LogPath 'host') 'game loaded:' $HostWaitSeconds
    Write-Host ("  host reached the world: {0}" -f $hostWorld)
    if (-not $hostWorld) { throw 'host never reached the world' }
    $companion = Wait-ForLogMatch $hostProc (LogPath 'host') 'companion: created' 60
    Write-Host ("  host companion created: {0}" -f $companion)

    Write-Host '--- starting guest (join -> load -> select party 1) ---'
    $env:DAOLIB_INSTANCE = 'guest'; $env:DAOCOOP_ROLE = 'guest'; $env:DAOCOOP_DRIVE = '1'; $env:DAOCOOP_HANDOFF = '0'
    $env:DAOCOOP_JOIN = '1'; $env:DAOCOOP_ADD_COMPANION = '0'; $env:DAOCOOP_SELECT_PARTY = '1'
    $env:DAOCOOP_AUTOLOAD = '0'
    $guestProc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Focus-GameWindow $guestProc 25
    $joined = Wait-ForLogMatch $guestProc (LogPath 'guest') 'game loaded: slot 91' $GuestWaitSeconds
    Write-Host ("  guest joined (host save loaded): {0}" -f $joined)
    $selected = Wait-ForLogMatch $guestProc (LogPath 'guest') 'select: asking for a click on index 1' 60
    Write-Host ("  guest selected party index 1: {0}" -f $selected)
    $controlChanged = Wait-ForLogMatch $guestProc (LogPath 'guest') 'control: 0x[0-9a-f]+ -> 0x[0-9a-f]+' 60
    Write-Host ("  guest control changed: {0}" -f $controlChanged)

    Write-Host ("--- syncing two avatars for {0} s ---" -f $SyncSeconds)
    Start-Sleep -Seconds $SyncSeconds
}
finally {
    foreach ($p in @($hostProc, $guestProc)) {
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
    Start-Sleep -Seconds 3
}

Write-Host ''
Write-Host '=== results ==='
$verdict = $true
$areas = @{}
$ids = @{}
foreach ($name in 'host', 'guest') {
    $log = LogPath $name
    Write-Host "--- $name ---"
    if (-not (Test-Path $log)) { Write-Host '  no log'; $verdict = $false; continue }
    $lines = Get-Content $log
    $text = $lines -join "`n"
    $lastNet = $lines | Where-Object { $_ -match 'net role=' } | Select-Object -Last 1
    Write-Host ("  last net: {0}" -f ($lastNet -replace '^\[[^\]]+\] \[[^\]]+\] \[info\] ', ''))
    if ($lastNet -match 'local=(0x[0-9a-f]+) remote=(0x[0-9a-f]+)') {
        $ids[$name] = @($Matches[1], $Matches[2])
    }
    if ($lastNet -match 'area (0x[0-9a-f]+)') { $areas[$name] = $Matches[1] }
    if ($name -eq 'host') {
        foreach ($pattern in 'companion: created', 'handoff: sent the slot folder') {
            $present = $text -match [regex]::Escape($pattern)
            Write-Host ("  {0}: {1}" -f $pattern, $present)
            if (-not $present) { $verdict = $false }
        }
        if ($text -match 'companion: created .* party list (\d+) member') {
            $party = [int]$Matches[1]
            Write-Host ("  party list after the hire: {0}" -f $party)
            if ($party -lt 2) { $verdict = $false }
        }
    }
    else {
        foreach ($pattern in 'game loaded: slot 91', 'select: asking for a click on index 1', 'control: ') {
            $present = $text -match [regex]::Escape($pattern)
            Write-Host ("  {0}: {1}" -f $pattern, $present)
            if (-not $present) { $verdict = $false }
        }
    }
    if ($lastNet -match 'puppet_applied ([0-9]+)') {
        $puppetApplies = [int]$Matches[1]
        Write-Host ("  puppet applies: {0}" -f $puppetApplies)
        if ($puppetApplies -lt $MinPuppetApplies) { $verdict = $false }
    }
    else { Write-Host '  no puppet_applied count'; $verdict = $false }
    if ($lastNet -match 'puppet_distance ([0-9.]+)') {
        $puppetDistance = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
        Write-Host ("  puppet distance: {0}" -f $puppetDistance)
        if ($puppetDistance -gt $MaxDistance) { $verdict = $false }
    }
}

if ($ids.Count -eq 2) {
    $hostIds = $ids['host']; $guestIds = $ids['guest']
    $distinct = ($hostIds[0] -ne $guestIds[0])
    # host's remote is the guest's controlled creature and vice versa
    $crossed = ($hostIds[1] -eq $guestIds[0]) -and ($guestIds[1] -eq $hostIds[0])
    Write-Host ("distinct controlled ids: {0}; each side sees the other's creature: {1}" -f $distinct, $crossed)
    if (-not $distinct) { $verdict = $false }
    if (-not $crossed) { $verdict = $false }
}
else { Write-Host 'id pairs missing from a net line'; $verdict = $false }
if ($areas.Count -eq 2) {
    $sameArea = $areas['host'] -eq $areas['guest']
    Write-Host ("same area in both games: {0} (host {1}, guest {2})" -f $sameArea, $areas['host'], $areas['guest'])
    if (-not $sameArea) { $verdict = $false }
}
else { Write-Host 'area id missing from a net line'; $verdict = $false }

Write-Host ''
Write-Host ("VERDICT: {0}" -f ($(if ($verdict) { 'PASS' } else { 'FAIL' })))
if (-not $verdict) { exit 1 }

