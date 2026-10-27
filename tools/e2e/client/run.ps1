<#
.SYNOPSIS
  Run an LiftOff scenario in the real game client (LeviLauncher instance).

.DESCRIPTION
  1. (-Build) builds bin/client/LiftOff via tools/build.ps1 -Target client (before
     taking the lock, so the game is not held while compiling).
  2. Takes a game lock: slot 1 (the LeviLauncher instance) uses
     %APPDATA%\levilauncher.exe\bei-e2e.lock, shared with BEI's and BeetrootSkin's harnesses;
     slots 2.. (-Slot, headless only) are clone instances with a lock each. Waits while every
     allowed slot is busy; gives up after -LockTimeoutMinutes with exit code 2. Remote modes
     start the BDS host only after that, so a queued run holds no server slot.

  Input is headless by default: the LeviHeadlessInput mod (https://github.com/LiteLDev/LeviHeadlessInput,
  %USERPROFILE%\Desktop\LeviHeadlessInput; $env:LEVI_HEADLESS_INPUT_DIR overrides its bin folder) is
  deployed with ours and every click, key, text and screenshot goes through its control pipe. The
  game stays in the background and never takes the real mouse, keyboard or foreground, so the PC
  stays usable during a run. -Foreground uses real SendInput as before (slot 1 only).
  3. Closes a running game, restores mods a killed run left aside, deploys the mod to
     mods\LiftOff, moves every other mod (e.g. BEI) aside, installs the LO-Test world.
  4. Runs optional scenarios/<name>.prelaunch.ps1, launches the game, waits for
     "LiftOff enabled", enters the world, dot-sources scenarios/<name>.ps1.
  5. Fails on: scenario exception, failing/empty/unfinished selftests, ERR/FTL lines from
     LiftOff/LeviLamina/CrashLogger.
  6. Always: closes the game, removes the LO-Test world and our mod, restores the other mods,
     runs scenarios/<name>.restore.ps1, releases the lock.
  Output: tools/e2e/client/out/<scenario>/ (screenshots, client.log, selftest.json). Look at the
  screenshots.

  Modes (-Mode):
    local           (default) the LO-Test local world: the integrated server runs the mod too.
    remote-mod      starts a BDS slot with the server build (tools/e2e/server, scenario
                    client-host), joins it through the Servers tab; the client runs the mod.
    remote-vanilla  same BDS, but our client mod is moved aside: a vanilla client (plus
                    LeviLamina) that only gets the server's resource pack: what players without the
                    client mod see.
  In remote modes the BDS console log is merged into the session log ("[host] " lines) and
  Send-HostConsole runs server console commands.

.EXAMPLE
  pwsh tools/e2e/client/run.ps1 -Scenario smoke -Build
  pwsh tools/e2e/client/run.ps1 -Scenario smoke -Mode remote-mod
  pwsh tools/e2e/client/run.ps1 -Scenario smoke -KeepOpen   # play around; close the game to finish
#>
param(
    [Parameter(Mandatory)] [string]$Scenario,
    [switch]$Build,
    [switch]$KeepOpen,          # leave the game running; the harness waits until you close it, then cleans up
    [switch]$KeepOtherMods,     # do not move other mods (BEI, ...) aside
    [string]$ModDir,            # default: <repo>/bin/client/LiftOff
    [ValidateSet('local', 'remote-mod', 'remote-vanilla')][string]$Mode = 'local',
    [int]$LockTimeoutMinutes = 120,  # give up (exit 2, "NOT RUN") if the game lock is not free by then; 0 = wait forever
    [int]$SlotWaitMinutes = 3,       # remote modes: no server slot by then -> release the game lock and queue again
    [int]$HostTimeoutMinutes = 15,   # remote modes: max wait for the BDS host (slot + boot), game lock held
    [switch]$HeadlessTrace,          # LeviHeadlessInput trace mode: log every key/mouse event reaching the game's input layer
    [switch]$Foreground,             # real SendInput, game in the foreground (takes over the PC); slot 1 only
    # Game instances this run may use (headless only); the first free one is taken. Default:
    # $env:LEVI_E2E_SLOTS ("1,2,3") or 1. Slots 2.. are clones (versions\<base>-hi<N>) made on first
    # use, so several runs can play at once.
    [int[]]$Slot
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"

if (-not $Foreground) {
    $shim = Join-Path $script:HeadlessInputDir 'harness-shim.ps1'
    if (-not (Test-Path $shim)) {
        throw "LeviHeadlessInput is not built ($shim missing). Build it (xmake in its project folder), set `$env:LEVI_HEADLESS_INPUT_DIR, or pass -Foreground to use the real mouse and keyboard."
    }
    # Replaces Click/Press-Key/Type-Text/Scroll/Move-Mouse/Focus-Game/Assert-GameForeground/
    # Set-GameWindowSize and routes [BsE2E.Native]::Key/MouseButton/... (used by scenarios)
    # through the mod's pipe.
    . $shim
    Register-HeadlessNative ([BsE2E.Native])
}
$slots = if ($Foreground) { @(1) } elseif ($Slot) { $Slot } elseif ($env:LEVI_E2E_SLOTS) {
    @($env:LEVI_E2E_SLOTS -split '[,; ]+' | Where-Object { $_ } | ForEach-Object { [int]$_ }) } else { @(1) }

$scenarioFile = Join-Path $PSScriptRoot "scenarios/$Scenario.ps1"
if (-not (Test-Path $scenarioFile)) { throw "Unknown scenario: $scenarioFile" }
if (-not $ModDir) { $ModDir = Join-Path $script:RepoRoot "bin/client/$script:ModName" }
$script:Unit = $Scenario
# The output folder is fixed once the game lock (and so the instance slot) is ours: slot 1 keeps
# out/<scenario>, slots 2.. use out/<scenario>-slot<N> so parallel runs of one scenario don't collide.
$outDir = Join-Path $script:OutRoot $Scenario
function Set-RunOutDir {
    $script:Unit = if ($script:InstanceSlot -gt 1) { "$Scenario-slot$($script:InstanceSlot)" } else { $Scenario }
    $dir = Join-Path $script:OutRoot $script:Unit
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force $dir | Out-Null
    return $dir
}

$failures = [System.Collections.Generic.List[string]]::new()
$script:Mode   = $Mode
$remote        = $Mode -ne 'local'
$hostInfo      = $null
$lockTimeout   = $null   # set when the run could not start: "NOT RUN", exit 2
$lockAcquired  = $false
$script:HostDir = $null

# Starts the BDS host (server harness, scenario client-host) in a free server slot and waits until
# it is ready. Called with the game lock held, so a queued run does not hold a slot meanwhile.
# Returns $null (host killed) when no slot became free within -SlotWaitMinutes: the caller then
# gives the game lock back instead of blocking the game while every slot is busy.
function Start-RemoteHost {
    $script:HostDir = Join-Path $outDir 'host'
    New-Item -ItemType Directory -Force $script:HostDir | Out-Null
    $env:LO_E2E_HOST_DIR = $script:HostDir
    $env:LO_E2E_HOST_OWNER_PID = "$PID"
    $serverRun = Join-Path $PSScriptRoot '../server/run.ps1'
    $harnessLog = Join-Path $outDir 'host-harness.log'
    # Own server output folder per client run (out/client-host-<unit>), so parallel remote runs don't collide.
    $script:HostOutName = "client-host-$script:Unit"
    $script:HostProc = Start-Process pwsh -ArgumentList @('-NoProfile', '-File', $serverRun, '-Scenario', 'client-host', '-OutName', $script:HostOutName, '-Quiet', '-TimeoutSec', '7200') `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $harnessLog -RedirectStandardError (Join-Path $outDir 'host-harness.err')
    Write-Step "starting the BDS host (pid $($script:HostProc.Id))"
    $ready = Join-Path $script:HostDir 'ready.json'
    $start = Get-Date
    $deadline = $start.AddMinutes($HostTimeoutMinutes)
    $slotDeadline = $start.AddMinutes($SlotWaitMinutes)
    $nextReport = $start.AddSeconds(60)
    while (-not (Test-Path $ready)) {
        if ($script:HostProc.HasExited) { throw "the BDS host exited early (see $harnessLog)" }
        if ((Get-Date) -gt $slotDeadline -and -not (Select-String -Path $harnessLog -Pattern 'acquired server slot' -Quiet -ErrorAction SilentlyContinue)) {
            Write-Step "no free server slot within $SlotWaitMinutes min; stopping the host"
            $script:HostProc.Kill($true); $script:HostProc.WaitForExit(15000) | Out-Null
            $script:HostProc = $null
            return $null
        }
        if ((Get-Date) -gt $deadline) { throw "the BDS host did not get ready in $HostTimeoutMinutes min (see $harnessLog)" }
        if ((Get-Date) -ge $nextReport) {
            $last = Get-Content $harnessLog -Tail 1 -ErrorAction SilentlyContinue
            Write-Step ("waiting for the BDS host ({0:N1} min): {1}" -f ((Get-Date) - $start).TotalMinutes, $last)
            $nextReport = (Get-Date).AddSeconds(60)
        }
        Start-Sleep -Seconds 2
    }
    $info = Get-Content $ready -Raw | ConvertFrom-Json
    if (-not $info.port) { throw "bad $ready" }
    $script:HostLog = Join-Path $PSScriptRoot "../server/out/$script:HostOutName/server.log"
    Write-Step "BDS host ready on port $($info.port) (slot $($info.slot))"
    return $info
}

# Asks the host to stop (it shuts the server down and releases its slot); does not wait.
function Request-HostStop {
    if ($script:HostProc -and $script:HostDir -and -not $script:HostProc.HasExited) {
        Set-Content -Path (Join-Path $script:HostDir 'stop') -Value 'stop'
    }
}

$script:HostProc = $null
$script:HostSlotTimedOut = $false
try {
    if ($Build) { Invoke-ModBuild client; if ($remote) { Invoke-ModBuild server } }
    # Game lock first, then the BDS host: a run queued for the game holds no server slot. If no
    # slot is free, give the game lock back and queue again (until -LockTimeoutMinutes).
    $lockDeadline = if ($LockTimeoutMinutes -gt 0) { (Get-Date).AddMinutes($LockTimeoutMinutes) } else { [datetime]::MaxValue }
    while ($true) {
        $left = if ($LockTimeoutMinutes -gt 0) { [Math]::Max(1, [int][Math]::Ceiling(($lockDeadline - (Get-Date)).TotalMinutes)) } else { 0 }
        Enter-E2ELock -TimeoutMinutes $left -Slots $slots
        $outDir = Set-RunOutDir
        if (-not $remote) { break }
        try { $hostInfo = Start-RemoteHost } catch { Exit-E2ELock; throw }
        if ($hostInfo) { break }
        Exit-E2ELock
        if ((Get-Date) -gt $lockDeadline) { $script:HostSlotTimedOut = $true; throw "no free server slot whenever the game lock was ours (within $LockTimeoutMinutes min)" }
        Write-Step "released the game lock; queueing again in 30 s"
        Start-Sleep -Seconds 30
    }
    $lockAcquired = $true
    try {
        # A clone instance is made on first use (cheap: hard links); it keeps its own mods and data.
        if ($script:InstanceSlot -gt 1) { & (Join-Path $script:HeadlessInputDir 'clone-instance.ps1') -Slot $script:InstanceSlot -Source $script:BaseVersion }
        if (Get-GameProcess) { Write-Step "closing a running game first"; Stop-Game }
        Restore-OtherMods
        if ($Mode -eq 'remote-vanilla') {
            Remove-DeployedMod # vanilla client: only LeviLamina (+ CrashLogger)
        } else {
            Deploy-Mod $ModDir
        }
        if (-not $KeepOtherMods) { Move-OtherModsAside }
        Remove-HeadlessInputLeftover   # of a killed run
        if ($script:Headless) { Install-HeadlessInputMod -ModsDir $script:ModsDir -PipeName $script:HeadlessPipe -Trace:$HeadlessTrace }
        if ($remote) { Set-ExternalServer $hostInfo.port } else {
            Restore-TestWorld
            # Single-player test world: games of all slots share one signaling-service ID, and
            # worlds opened for multiplayer knock each other out ("An error has occurred", "Blaze").
            if ($script:Headless) { Set-WorldSinglePlayer (Join-Path (Get-WorldsDir) $script:WorldName) }
        }
        Reset-CameraPerspective
        $prelaunch = Join-Path $PSScriptRoot "scenarios/$Scenario.prelaunch.ps1"
        if (Test-Path $prelaunch) { . $prelaunch }
        try {
            # Joining the local world sometimes fails with "Connection timed out (Kelp)" and,
            # once it does, keeps failing in that game session: retry with a fresh game.
            for ($launch = 1; ; $launch++) {
                if ($remote) { Clear-OurServerEntries }
                Start-Game | Out-Null
                Wait-GameReady -WithoutMod:($Mode -eq 'remote-vanilla')
                try {
                    if ($remote) { Enter-RemoteServer -Attempts 1 } else { Enter-TestWorld -Attempts 2 }
                    break
                } catch {
                    if ($launch -ge 3) { throw }
                    Write-Step "could not enter the world ($_); restarting the game (launch $($launch + 1)/3)"
                    Stop-Game
                    Start-Sleep -Seconds 3
                }
            }
            Write-Step "running scenario '$Scenario'"
            . $scenarioFile
        } catch {
            $failures.Add("scenario: $_")
            Write-Host "[e2e] $($_.ScriptStackTrace)" -ForegroundColor DarkGray
            try { Save-Screenshot 'failure' | Out-Null } catch {}
            Get-LeviLog | Select-Object -Last 40 | ForEach-Object { Write-Host "  | $_" }
        }
        $errs = Get-ErrorLines
        if ($errs) {
            $failures.Add("$($errs.Count) ERR/FTL line(s) from $script:ModName/LeviLamina/CrashLogger")
            $errs | Select-Object -First 20 | ForEach-Object { Write-Host "  ! $_" -ForegroundColor Red }
        }
        foreach ($p in Get-SelfTestProblems) { $failures.Add($p) }
        Save-SelfTestReport (Join-Path $outDir 'selftest.json')
        Get-LeviLog | Set-Content -Path (Join-Path $outDir 'client.log') -Encoding UTF8
    } finally {
        if ($KeepOpen -and (Get-GameProcess)) {
            Write-Step "-KeepOpen: the game stays open; close it to finish the run (lock still held)"
            if ($script:Headless) {
                # Hand the game to the human: real mouse and keyboard work again.
                try { Set-HeadlessBackground $false | Out-Null; Write-Step "headless input off: the game takes real input again" } catch {}
            }
            while (Get-GameProcess) { Start-Sleep -Seconds 2 }
        }
        Stop-Game
        Request-HostStop # the host shuts down while we clean up
        if ($remote) { Restore-ExternalServers } else { Remove-TestWorld }
        Remove-DeployedMod
        Remove-HeadlessInputLeftover
        Restore-OtherMods
        $restore = Join-Path $PSScriptRoot "scenarios/$Scenario.restore.ps1"
        # Dot-sourced (like the prelaunch script) so it sees this run's slot ($script:VersionName,
        # Get-GameDataDir, ...); a failing restore must not keep the lock.
        if (Test-Path $restore) { try { . $restore } catch { Write-Host "[e2e] restore script failed: $_" -ForegroundColor Red } }
        Exit-E2ELock
    }
} catch {
    if (($script:LockTimedOut -and -not $lockAcquired) -or $script:HostSlotTimedOut) {
        $lockTimeout = "$_"
    } else {
        $failures.Add("harness: $_")
        Write-Host "[e2e] $($_.ScriptStackTrace)" -ForegroundColor DarkGray
    }
} finally {
    # Runs on success, failure, timeout and Ctrl+C: never leave the host (and its slot) behind.
    if ($script:HostProc) {
        $hp = $script:HostProc
        if (-not $hostInfo) {
            # Not ready yet (still waiting for a slot or booting): nobody needs it, kill it now.
            if (-not $hp.HasExited) { Write-Step "killing the BDS host that never got ready"; $hp.Kill($true) }
        } else {
            Request-HostStop
            if (-not $hp.WaitForExit(120000)) { $hp.Kill($true); $failures.Add('BDS host did not stop') }
            elseif ($hp.ExitCode -ne 0) { $failures.Add("BDS host failed (exit $($hp.ExitCode), see tools/e2e/server/out/$script:HostOutName/)") }
        }
        Remove-Item Env:LO_E2E_HOST_DIR, Env:LO_E2E_HOST_OWNER_PID -ErrorAction SilentlyContinue
    }
}

if ($lockTimeout) {
    Write-Host "[e2e] client scenario '$Scenario' NOT RUN (game lock or server slot not free in time; nothing was tested)" -ForegroundColor Yellow
    Write-Host "[e2e]   $lockTimeout" -ForegroundColor Yellow
    exit 2
}

if ($failures.Count -gt 0) {
    Write-Host "[e2e] client scenario '$Scenario' FAILED:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "[e2e]   - $_" -ForegroundColor Red }
    Write-Host "[e2e] output: $outDir"
    exit 1
}
Write-Host "[e2e] client scenario '$Scenario' PASSED (screenshots: $outDir)" -ForegroundColor Green
exit 0
