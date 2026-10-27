# LiftOff server (BDS + LeviLamina) e2e helpers. Dot-sourced by run.ps1; scenarios
# use the helpers in the "Scenario API" section.

. "$PSScriptRoot/../common/common.ps1"

$script:ServerDir  = $PSScriptRoot
$script:E2EHome    = Get-E2EHome
$script:BaseDir    = Join-Path $script:E2EHome 'bds-base'
$script:SlotsDir   = Join-Path $script:E2EHome 'slots'
$script:ModName    = 'LiftOff'
# Number of parallel server slots. Each slot has its own bedrock_server_mod.exe path, and Windows
# Firewall asks once per new executable path, so keep this small (see README).
$script:SlotCount  = if ($env:LO_E2E_SERVER_SLOTS) { [int]$env:LO_E2E_SERVER_SLOTS } else { 4 }

if (-not ('BsE2E.LineSink' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
namespace BsE2E {
// Collects a process' stdout/stderr lines on background threads (no PowerShell runspace
// involved), strips ANSI colors and mirrors every line to a log file as it arrives.
public class LineSink {
    static readonly Regex Ansi = new Regex("\x1b\\[[0-9;?]*[A-Za-z]", RegexOptions.Compiled);
    readonly List<string> lines = new List<string>();
    readonly StreamWriter file;
    public LineSink(string logPath) {
        file = new StreamWriter(new FileStream(logPath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite), new UTF8Encoding(false));
        file.AutoFlush = true;
    }
    public void Attach(Process p) {
        p.OutputDataReceived += (s, e) => { if (e.Data != null) Add(e.Data); };
        p.ErrorDataReceived  += (s, e) => { if (e.Data != null) Add("[stderr] " + e.Data); };
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
    }
    public void Add(string line) {
        line = Ansi.Replace(line, "");
        lock (lines) { lines.Add(line); try { file.WriteLine(line); } catch (ObjectDisposedException) {} }
    }
    public int Count { get { lock (lines) return lines.Count; } }
    public string[] Since(int index) {
        lock (lines) {
            if (index < 0) index = 0;
            if (index >= lines.Count) return new string[0];
            return lines.GetRange(index, lines.Count - index).ToArray();
        }
    }
    public void Close() { lock (lines) file.Dispose(); }
}
}
'@
}

# ---------------------------------------------------------------------------------------
# Slots: <home>\slots\<n>\ is a full server directory; <home>\slots\<n>.lock is held for the
# whole run. Different slots run concurrently on different ports.
# ---------------------------------------------------------------------------------------
function Enter-ServerSlot([int]$TimeoutMinutes = 120, [int]$Slot = -1) {
    New-Item -ItemType Directory -Force $script:SlotsDir | Out-Null
    $candidates = if ($Slot -ge 0) { @($Slot) } else { 0..($script:SlotCount - 1) }
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $announced = $false
    $nextOrphanCheck = Get-Date
    while ($true) {
        foreach ($n in $candidates) {
            $s = Enter-FileLockOnce (Join-Path $script:SlotsDir "$n.lock")
            if ($s) {
                $script:SlotLock = $s
                $script:Slot = $n
                $script:SlotDir = Join-Path $script:SlotsDir "$n"
                Write-Step "acquired server slot $n ($script:SlotDir)"
                return
            }
        }
        if ((Get-Date) -ge $nextOrphanCheck) {
            # All busy: free slots whose hosts belong to a run that is gone.
            $killed = Stop-OrphanedHosts $candidates
            $nextOrphanCheck = (Get-Date).AddSeconds(60)
            if ($killed -gt 0) { Start-Sleep -Seconds 2; continue }
        }
        if (-not $announced) { Write-Step "all $($candidates.Count) server slot(s) busy; waiting"; $announced = $true }
        if ((Get-Date) -gt $deadline) { throw "Timed out waiting for a free server slot" }
        Start-Sleep -Seconds 5
    }
}

# Hosts older than this are killed when the pid recorded as their slot-lock holder is dead.
$script:HostMaxAgeMinutes = if ($env:LO_E2E_HOST_MAX_AGE_MIN) { [int]$env:LO_E2E_HOST_MAX_AGE_MIN } else { 45 }

# Kills BDS hosts in busy slots that no live run owns. A server in slot n is started by a
# server/run.ps1 (the harness, which holds <n>.lock); for the client harness's remote modes that
# harness runs scenario client-host and is itself started by client/run.ps1 (the owner). A host is
# an orphan when
#   - its harness is gone, or
#   - it is a client-host and its owning client run is gone, or
#   - it is older than $script:HostMaxAgeMinutes and the pid in <n>.lock.owner is dead.
# A host whose harness and owner are alive is never touched. Returns the number of hosts killed.
function Stop-OrphanedHosts([int[]]$Slots, [switch]$DryRun) {
    $procs = @(Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, CreationDate, CommandLine, ExecutablePath -ErrorAction SilentlyContinue)
    $byId = @{}
    foreach ($p in $procs) { $byId[[int]$p.ProcessId] = $p }
    # The parent, if it is still the process that started $p (pids are reused on Windows).
    $parentOf = {
        param($p)
        $q = $byId[[int]$p.ParentProcessId]
        if ($q -and $q.CreationDate -le $p.CreationDate) { $q } else { $null }
    }
    $killed = 0
    foreach ($bds in $procs) {
        if (-not $bds.ExecutablePath -or $bds.ExecutablePath -notmatch '\\slots\\(\d+)\\bedrock_server_mod\.exe$') { continue }
        $n = [int]$Matches[1]
        if ($n -notin $Slots) { continue }
        $slotExe = Join-Path (Join-Path $script:SlotsDir "$n") 'bedrock_server_mod.exe'
        if ($bds.ExecutablePath -ne $slotExe) { continue }   # another E2E home: not ours to judge
        $harness = & $parentOf $bds
        $reason = $null
        if (-not $harness) {
            $reason = 'its harness is gone'
        } elseif ($harness.CommandLine -match '-Scenario\s+client-host\b') {
            if (-not (& $parentOf $harness)) { $reason = "its client run (pid $($harness.ParentProcessId)) is gone" }
        }
        $ageMin = ((Get-Date) - $bds.CreationDate).TotalMinutes
        if (-not $reason -and $ageMin -gt $script:HostMaxAgeMinutes) {
            # The real lock holder (Restart Manager), not the best-effort .owner file. Unknown = keep.
            $holders = @(try { Get-FileHolderPids (Join-Path $script:SlotsDir "$n.lock") } catch { @() })
            if ($holders -and -not @($holders | Where-Object { $byId[[int]$_] })) {
                $reason = "it is $([int]$ageMin) min old and its slot-lock holder (pid $($holders -join ',')) is dead"
            }
        }
        if (-not $reason) { if ($DryRun) { Write-Step "slot $($n): host pid $($bds.ProcessId) ($([int]$ageMin) min) is owned by a live run" }; continue }
        if ($DryRun) { Write-Step "slot $($n): would kill host pid $($bds.ProcessId): $reason"; continue }
        # Kill the harness tree when it is still around (it holds the slot lock), else the server.
        $target = if ($harness) { $harness.ProcessId } else { $bds.ProcessId }
        Write-Step "slot $($n): killing orphaned host (server pid $($bds.ProcessId), $([int]$ageMin) min old): $reason"
        try {
            $p = [System.Diagnostics.Process]::GetProcessById([int]$target)
            $p.Kill($true); $p.WaitForExit(15000) | Out-Null
            $killed++
        } catch { Write-Step "could not kill pid $($target): $_" }
    }
    return $killed
}

function Exit-ServerSlot {
    if ($script:SlotLock) { Exit-FileLock $script:SlotLock; $script:SlotLock = $null; Write-Step "released server slot $script:Slot" }
}

# Kill server processes left behind in this slot by a harness that was killed (we hold the
# slot lock, so anything still running from the slot directory is an orphan).
function Stop-SlotOrphans {
    $suffix = "\lo-e2e\slots\$script:Slot\bedrock_server_mod.exe"
    $exe = Join-Path $script:SlotDir 'bedrock_server_mod.exe'
    Get-Process -Name 'bedrock_server_mod' -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and ($_.Path -eq $exe -or $_.Path.EndsWith($suffix, [StringComparison]::OrdinalIgnoreCase))
    } | ForEach-Object {
        Write-Step "killing orphaned server pid $($_.Id) in slot $script:Slot"
        $_.Kill(); $_.WaitForExit(10000) | Out-Null
    }
}

function Get-ModsDirName([string]$Root) {
    $cfg = Join-Path $Root 'PreLoaderConfig.json'
    if (Test-Path $cfg) {
        try { $p = (Get-Content $cfg -Raw | ConvertFrom-Json).modsPath; if ($p) { return ($p -replace '^\./', '' -replace '[/\\]+$', '') } } catch {}
    }
    if (Test-Path (Join-Path $Root 'plugins\LeviLamina')) { return 'plugins' }
    return 'mods'
}

function Invoke-Robocopy([string]$From, [string]$To, [string[]]$Extra = @()) {
    if (-not (Test-Path $From)) { if (Test-Path $To) { Remove-Item -Recurse -Force $To }; return }
    & robocopy $From $To /MIR /MT:16 /R:2 /W:1 /NFL /NDL /NJH /NJS /NP @Extra | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy $From -> $To failed (exit $LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
}

# Bring the slot in sync with bds-base and reset all per-run state (world, logs, mods, configs).
function Initialize-ServerSlot([string]$ModDir) {
    Stop-SlotOrphans
    $slot = $script:SlotDir
    New-Item -ItemType Directory -Force $slot | Out-Null
    $baseStamp = Get-Content (Join-Path $script:BaseDir '.lo-e2e-installed') -Raw
    $slotStamp = Join-Path $slot '.lo-slot-stamp'
    $fresh = -not ((Test-Path $slotStamp) -and (Get-Content $slotStamp -Raw) -eq $baseStamp)
    if ($fresh) {
        Write-Step "populating slot $script:Slot from $script:BaseDir"
        Remove-Item $slotStamp -ErrorAction SilentlyContinue
        # Large immutable binaries are hard-linked (fast, no disk use), everything else copied.
        foreach ($f in Get-ChildItem $script:BaseDir -File) {
            if ($f.Name -in 'bedrock_server.exe.bak', 'bdsdown.exe', 'PeEditor.exe', 'tooth_lock.json', '.lo-e2e-installed') { continue }
            $dst = Join-Path $slot $f.Name
            if (Test-Path $dst) { Remove-Item -Force $dst }
            $linked = $false
            if ($f.Extension -in '.exe', '.dll' -or $f.Name -eq 'bedrock_runtime_data') {
                try { New-Item -ItemType HardLink -Path $dst -Target $f.FullName -ErrorAction Stop | Out-Null; $linked = $true } catch {}
            }
            if (-not $linked) { Copy-Item $f.FullName $dst }
        }
    }
    # Per-run reset. /MIR only copies what changed, so this is quick after the first run.
    $mods = Get-ModsDirName $script:BaseDir
    foreach ($d in 'behavior_packs', 'resource_packs', 'definitions', 'data', 'config', $mods) {
        Invoke-Robocopy (Join-Path $script:BaseDir $d) (Join-Path $slot $d)
    }
    foreach ($d in 'worlds', 'logs', 'development_behavior_packs', 'development_resource_packs', 'development_skin_packs', 'world_templates') {
        $p = Join-Path $slot $d
        if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    }
    foreach ($f in 'server.properties', 'allowlist.json', 'permissions.json', 'packetlimitconfig.json') {
        $src = Join-Path $script:BaseDir $f
        if (Test-Path $src) { Copy-Item -Force $src (Join-Path $slot $f) }
    }
    # PreLoaderConfig.json (modsPath etc.) is regenerated by PreLoader with defaults unless the
    # base has one; keep the slot consistent with the base either way.
    Remove-Item (Join-Path $slot 'PreLoaderConfig.json'), (Join-Path $slot 'packet-statistics.txt') -ErrorAction SilentlyContinue
    $plc = Join-Path $script:BaseDir 'PreLoaderConfig.json'
    if (Test-Path $plc) { Copy-Item -Force $plc (Join-Path $slot 'PreLoaderConfig.json') }
    if ($fresh) { Set-Content -Path $slotStamp -Value $baseStamp -NoNewline -Encoding UTF8 }

    # Deploy the mod.
    $dest = Join-Path $slot "$mods\$script:ModName"
    if (-not (Test-Path (Join-Path $ModDir "$script:ModName.dll"))) { throw "Build output not found: $ModDir (run 'pwsh tools/build.ps1 -Target server' or pass -Build)" }
    Copy-Item -Recurse -Force $ModDir $dest
    Set-TestModConfig $dest
    Write-Step "deployed $ModDir -> $dest"
}

function Test-UdpPortFree([int]$Port) {
    try { $u = [System.Net.Sockets.UdpClient]::new($Port); $u.Dispose(); return $true } catch { return $false }
}

# Write server.properties for the fixture world. $Overrides (hashtable) wins over defaults.
# Slot n owns UDP ports 19600+10n .. 19609+10n, so slots never race for the same port.
function Set-ServerProperties([hashtable]$Overrides = @{}) {
    $band = 19600 + 10 * $script:Slot
    $port = $null
    foreach ($p in $band, ($band + 2), ($band + 4), ($band + 6), ($band + 8)) {
        if ((Test-UdpPortFree $p) -and (Test-UdpPortFree ($p + 1))) { $port = $p; break }
    }
    if (-not $port) { throw "no free UDP port pair in $band..$($band + 9) for slot $script:Slot" }
    $script:ServerPort = $port
    $props = [ordered]@{
        'server-name'                       = "LO-E2E-slot$script:Slot"
        'level-name'                        = 'LO-Test'
        'level-type'                        = 'FLAT'
        'level-seed'                        = '20260521'
        'gamemode'                          = 'survival'
        'difficulty'                        = 'peaceful'
        'allow-cheats'                      = 'true'
        'server-port'                       = "$port"
        'server-portv6'                     = "$($port + 1)"
        'enable-lan-visibility'             = 'false'
        # Real clients (client e2e remote modes) join 127.0.0.1:<port> directly: RakNet (the
        # NetherNet default needs the online signaling service) and no allow list.
        'transport'                         = 'raknet'
        'allow-list'                        = 'false'
        'max-players'                       = '20'
        'player-idle-timeout'               = '0'
        'default-player-permission-level'   = 'operator'
        'texturepack-required'              = 'true'
        'content-log-file-enabled'          = 'false'
        'content-log-console-output-enabled' = 'true'
        'emit-server-telemetry'             = 'false'
        'view-distance'                     = '10'
        'tick-distance'                     = '4'
    }
    foreach ($k in $Overrides.Keys) { $props[$k] = "$($Overrides[$k])" }
    $file = Join-Path $script:SlotDir 'server.properties'
    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path $file) { $lines.AddRange([string[]](Get-Content $file)) }
    foreach ($k in $props.Keys) {
        $idx = -1
        for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match "^\s*$([regex]::Escape($k))\s*=") { $idx = $i; break } }
        if ($idx -ge 0) { $lines[$idx] = "$k=$($props[$k])" } else { $lines.Add("$k=$($props[$k])") }
    }
    Set-Content -Path $file -Value $lines -Encoding UTF8
    Write-Step "server.properties: level LO-Test (flat, peaceful, cheats), port $port/$($port + 1)"
}

# ---------------------------------------------------------------------------------------
# Server process
# ---------------------------------------------------------------------------------------
$script:Proc = $null
$script:Sink = $null
$script:Echoed = 0
$script:EchoOutput = $true
$script:Deadline = [datetime]::MaxValue   # global scenario deadline (run.ps1 -TimeoutSec)

function Start-Server([string]$LogPath) {
    $exe = Join-Path $script:SlotDir 'bedrock_server_mod.exe'
    $psi = [System.Diagnostics.ProcessStartInfo]::new($exe)
    $psi.WorkingDirectory = $script:SlotDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $psi.CreateNoWindow = $true
    New-Item -ItemType Directory -Force (Split-Path $LogPath) | Out-Null
    $script:Sink = [BsE2E.LineSink]::new($LogPath)
    $script:Echoed = 0
    $script:Proc = [System.Diagnostics.Process]::Start($psi)
    $script:Sink.Attach($script:Proc)
    Write-Step "server started (pid $($script:Proc.Id)), log -> $LogPath"
}

function Test-ServerRunning { $script:Proc -and -not $script:Proc.HasExited }

# Echo server output that has not been printed yet.
function Show-NewServerOutput {
    if (-not $script:Sink) { return }
    $new = $script:Sink.Since($script:Echoed)
    $script:Echoed += $new.Count
    if ($script:EchoOutput) { foreach ($l in $new) { Write-Host "  | $l" } }
}

function Stop-Server([int]$TimeoutSeconds = 60) {
    if (-not $script:Proc) { return $true }
    $graceful = $true
    if (-not $script:Proc.HasExited) {
        Write-Step "stopping server"
        try { $script:Proc.StandardInput.WriteLine('stop'); $script:Proc.StandardInput.Flush() } catch {}
        $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
        while (-not $script:Proc.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300; Show-NewServerOutput }
        if (-not $script:Proc.HasExited) {
            Write-Step "server did not stop within $TimeoutSeconds s; killing it"
            $script:Proc.Kill($true); $script:Proc.WaitForExit(10000) | Out-Null
            $graceful = $false
        }
    }
    # Give the async stdout readers a moment to drain (no unbounded WaitForExit(): a child that
    # inherited the pipes could keep them open forever).
    Start-Sleep -Milliseconds 500
    Show-NewServerOutput
    $script:ExitCode = $script:Proc.ExitCode
    Write-Step "server exited with code $script:ExitCode"
    return $graceful
}

# ---------------------------------------------------------------------------------------
# Scenario API
# ---------------------------------------------------------------------------------------

# Send one line to the server console, e.g. Send-Console 'give @a diamond 3'. No leading slash.
function Send-Console([string]$Command) {
    if (-not (Test-ServerRunning)) { throw "server is not running (cannot send '$Command')" }
    Write-Step "> $Command"
    $script:Proc.StandardInput.WriteLine($Command)
    $script:Proc.StandardInput.Flush()
}

# All server output lines of this run (ANSI colors stripped). -Since <mark> returns only lines
# logged after Get-LogMark returned <mark>.
function Get-ServerLog([int]$Since = 0) { if ($script:Sink) { $script:Sink.Since($Since) } else { @() } }

# Current position in the log; pass it to Wait-LogLine/Get-ServerLog -Since.
function Get-LogMark { if ($script:Sink) { $script:Sink.Count } else { 0 } }

# Wait for a line matching the regex (searching from -Since, default: whole run). Returns the
# first matching line. Throws on timeout or if the server exits.
function Wait-LogLine([string]$Pattern, [int]$TimeoutSeconds = 120, [int]$Since = 0) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    if ($script:Deadline -lt $deadline) { $deadline = $script:Deadline }
    $pos = $Since
    while ($true) {
        $lines = Get-ServerLog -Since $pos
        $pos += $lines.Count
        $hit = $lines | Select-String -Pattern $Pattern | Select-Object -First 1
        Show-NewServerOutput
        if ($hit) { return $hit.Line }
        if (-not (Test-ServerRunning)) {
            Start-Sleep -Milliseconds 300
            $hit = Get-ServerLog -Since $pos | Select-String -Pattern $Pattern | Select-Object -First 1
            if ($hit) { return $hit.Line }
            throw "server exited (code $($script:Proc.ExitCode)) while waiting for '$Pattern'"
        }
        if ((Get-Date) -gt $deadline) { throw "timed out after $TimeoutSeconds s waiting for log line '$Pattern'" }
        Start-Sleep -Milliseconds 250
    }
}

# Used by Invoke-SelfTest (common.ps1).
function Send-SelfTestCommand([string]$Command) { Send-Console $Command }
# Wait a number of seconds while echoing server output.
function Wait-Seconds([double]$Seconds) {
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) { Start-Sleep -Milliseconds 250; Show-NewServerOutput }
}
