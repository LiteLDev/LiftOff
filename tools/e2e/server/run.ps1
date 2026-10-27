<#
.SYNOPSIS
  Run an LiftOff scenario on a real Bedrock Dedicated Server + LeviLamina.

.DESCRIPTION
  1. (-Build) builds bin/server/LiftOff via tools/build.ps1 -Target server.
  2. Installs BDS + LeviLamina into the machine-wide cache on first use (install.ps1).
  3. Takes a free server slot (<home>\slots\<n>.lock; waits when all are busy). Slots run in
     parallel on different ports, so many worktrees can test at once.
  4. Resets the slot: syncs it with bds-base, deletes the world, deploys the mod, writes
     server.properties for a fresh flat/peaceful/cheats world "LO-Test" (fixed seed).
  5. Runs optional scenarios/<name>.prelaunch.ps1, boots bedrock_server_mod.exe with redirected
     stdin/stdout, waits for "LiftOff enabled" and "Server started", makes it
     always day / clear weather / no mobs.
  6. Dot-sources scenarios/<name>.ps1 (helpers: see lib.ps1 "Scenario API" and README).
  7. Stops the server with "stop" and fails the run on: scenario exception, failing/empty/
     unfinished selftests, ERR/FTL lines from LiftOff/LeviLamina/CrashLogger, crash
     dumps, non-zero exit code or a server that does not stop.
  Output: tools/e2e/server/out/<scenario>/server.log (full console), selftest.json, crash/.
  Exit code 0 = pass, 1 = fail.

.EXAMPLE
  pwsh tools/e2e/server/run.ps1 -Scenario smoke -Build
  pwsh tools/e2e/server/run.ps1 -Scenario full -TimeoutSec 1800
#>
param(
    [Parameter(Mandatory)] [string]$Scenario,
    [switch]$Build,
    [switch]$KeepOpen,                 # interactive: keep the server running and forward your console input
    [int]$TimeoutSec = 900,            # whole-scenario budget (boot excluded)
    [int]$BootTimeoutSec = 180,
    [string[]]$Property = @(),         # extra server.properties overrides: -Property gamemode=creative,difficulty=easy
    [string]$ModDir,                   # default: <repo>/bin/server/LiftOff
    [int]$Slot = -1,                   # force a specific slot
    [string]$OutName,                  # output folder name under out/ (default: the scenario name)
    [switch]$Quiet                     # do not echo server output
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$script:EchoOutput = -not $Quiet

$scenarioFile = Join-Path $PSScriptRoot "scenarios/$Scenario.ps1"
if (-not (Test-Path $scenarioFile)) { throw "Unknown scenario: $scenarioFile" }
if (-not $ModDir) { $ModDir = Join-Path $script:RepoRoot "bin/server/$script:ModName" }
if (-not $OutName) { $OutName = $Scenario }
$outDir = Join-Path $PSScriptRoot "out/$OutName"
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force $outDir | Out-Null
$script:Unit = $Scenario
$script:OutDir = $outDir

$failures = [System.Collections.Generic.List[string]]::new()
try {
    if ($Build) { Invoke-ModBuild server }
    & "$PSScriptRoot/install.ps1"
    Enter-ServerSlot -Slot $Slot
    try {
        Initialize-ServerSlot $ModDir
        $overrides = @{}
        foreach ($kv in ($Property -split ',' | Where-Object { $_.Trim() })) {
            if ($kv -notmatch '^\s*([^=]+?)\s*=(.*)$') { throw "bad -Property '$kv' (expected key=value)" }
            $overrides[$Matches[1]] = $Matches[2]
        }
        Set-ServerProperties $overrides
        $prelaunch = Join-Path $PSScriptRoot "scenarios/$Scenario.prelaunch.ps1"
        if (Test-Path $prelaunch) { . $prelaunch }

        Start-Server (Join-Path $outDir 'server.log')
        try {
            $script:Deadline = (Get-Date).AddSeconds($BootTimeoutSec)
            $hit = Wait-LogLine "$script:ModName enabled|Failed to (load|enable) mod $script:ModName" $BootTimeoutSec
            if ($hit -notmatch "$script:ModName enabled") { throw "LeviLamina failed to load $($script:ModName): $hit" }
            Wait-LogLine 'Server started' $BootTimeoutSec | Out-Null
            # Fixture world: always day, clear weather, no mob spawning.
            foreach ($c in 'gamerule dodaylightcycle false', 'time set noon', 'gamerule doweathercycle false', 'weather clear', 'gamerule domobspawning false') { Send-Console $c }
            Wait-Seconds 1

            $script:Deadline = (Get-Date).AddSeconds($TimeoutSec)
            Write-Step "running scenario '$Scenario'"
            . $scenarioFile
            $script:Deadline = [datetime]::MaxValue

            if ($KeepOpen) {
                if ([Console]::IsInputRedirected) {
                    Write-Step "-KeepOpen needs an interactive console; ignoring"
                } else {
                    Write-Step "-KeepOpen: type server commands, 'stop' to finish"
                    while (Test-ServerRunning) {
                        Show-NewServerOutput
                        $line = Read-Host 'bds'
                        if ($line -eq 'stop') { break }
                        if ($line) { Send-Console $line; Wait-Seconds 1 }
                    }
                }
            }
        } catch {
            $failures.Add("scenario: $_")
            Write-Host "[e2e] $($_.ScriptStackTrace)" -ForegroundColor DarkGray
        } finally {
            $script:Deadline = [datetime]::MaxValue
            if (-not (Stop-Server)) { $failures.Add('server did not stop gracefully (killed)') }
            $script:Sink.Close()
        }

        if ($script:ExitCode -ne 0) { $failures.Add("server exit code $script:ExitCode") }
        $errs = Get-ErrorLines
        if ($errs) {
            $failures.Add("$($errs.Count) ERR/FTL line(s) from $script:ModName/LeviLamina/CrashLogger")
            $errs | Select-Object -First 20 | ForEach-Object { Write-Host "  ! $_" -ForegroundColor Red }
        }
        $crashDirs = @(Join-Path $script:SlotDir 'logs\crash') | Where-Object { Test-Path $_ }
        $crashFiles = @($crashDirs | ForEach-Object { Get-ChildItem $_ -Recurse -File })
        if ($crashFiles) {
            $failures.Add("CrashLogger wrote $($crashFiles.Count) crash file(s) (copied to out/$OutName/crash)")
            New-Item -ItemType Directory -Force (Join-Path $outDir 'crash') | Out-Null
            $crashFiles | Copy-Item -Destination (Join-Path $outDir 'crash')
        }
        foreach ($p in Get-SelfTestProblems) { $failures.Add($p) }
        Save-SelfTestReport (Join-Path $outDir 'selftest.json')
    } finally {
        Exit-ServerSlot
    }
} catch {
    $failures.Add("harness: $_")
    Write-Host "[e2e] $($_.ScriptStackTrace)" -ForegroundColor DarkGray
}

if ($failures.Count -gt 0) {
    Write-Host "[e2e] server scenario '$Scenario' FAILED:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "[e2e]   - $_" -ForegroundColor Red }
    Write-Host "[e2e] log: $(Join-Path $outDir 'server.log')"
    exit 1
}
Write-Host "[e2e] server scenario '$Scenario' PASSED" -ForegroundColor Green
exit 0
