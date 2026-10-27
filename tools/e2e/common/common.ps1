# Helpers shared by the server and client e2e harnesses. Dot-source it.

$script:E2ERoot  = (Resolve-Path "$PSScriptRoot/..").Path
$script:RepoRoot = (Resolve-Path "$PSScriptRoot/../../..").Path

# Machine-wide cache/instance root. Override with $env:LO_E2E_HOME (e.g. to put it on another drive).
# NOTE: when this runs inside a packaged (MSIX) app such as the Claude desktop app, Windows
# virtualizes %LOCALAPPDATA%: files really live under
# %LOCALAPPDATA%\Packages\<package>\LocalCache\Local\lo-e2e. Every process started from the same
# app sees the same view, so this is transparent unless you browse the folder from Explorer.
function Get-E2EHome {
    $h = if ($env:LO_E2E_HOME) { $env:LO_E2E_HOME } else { Join-Path $env:LOCALAPPDATA 'lo-e2e' }
    New-Item -ItemType Directory -Force $h | Out-Null
    return $h
}

function Write-Step([string]$msg) { Write-Host "[e2e] $msg" }

# ---------------------------------------------------------------------------------------
# File locks. The lock is an exclusively opened file (FileShare.None), so the OS releases it
# when the holding process dies: there is no stale-lock state to clean up. The holder writes
# "pid/repo/time" to <lock>.owner (the lock file itself cannot be read while held), which is
# informational only. Other projects' harnesses (BEI, BeetrootSkin) take the game lock the same
# way but only write into the lock file itself, so the holder is found with the Restart Manager
# (which process has the file open) and <lock>.owner only names the repo when its pid matches.
# ---------------------------------------------------------------------------------------
# Compiled on first use (only while waiting for a lock), not on every harness start.
function Get-FileHolderPids([string]$Path) {
    if (-not ('BsE2E.FileHolders' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
namespace BsE2E {
public static class FileHolders {
    [StructLayout(LayoutKind.Sequential)] struct RM_UNIQUE_PROCESS { public int dwProcessId; public System.Runtime.InteropServices.ComTypes.FILETIME ProcessStartTime; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct RM_PROCESS_INFO {
        public RM_UNIQUE_PROCESS Process;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string strAppName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string strServiceShortName;
        public int ApplicationType; public uint AppStatus; public uint TSSessionId;
        [MarshalAs(UnmanagedType.Bool)] public bool bRestartable;
    }
    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)] static extern int RmStartSession(out uint h, int flags, StringBuilder key);
    [DllImport("rstrtmgr.dll")] static extern int RmEndSession(uint h);
    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)] static extern int RmRegisterResources(uint h, uint nFiles, string[] files, uint nApps, RM_UNIQUE_PROCESS[] apps, uint nSvc, string[] svcs);
    [DllImport("rstrtmgr.dll")] static extern int RmGetList(uint h, out uint needed, ref uint count, [In, Out] RM_PROCESS_INFO[] info, ref uint reasons);
    // Pids of the processes that have the file open (empty when unknown).
    public static int[] Get(string path) {
        var result = new List<int>();
        uint h;
        if (RmStartSession(out h, 0, new StringBuilder(64)) != 0) return result.ToArray();
        try {
            if (RmRegisterResources(h, 1, new[] { path }, 0, null, 0, null) != 0) return result.ToArray();
            uint needed = 0, count = 0, reasons = 0;
            RM_PROCESS_INFO[] info = null;
            for (int attempt = 0; attempt < 5; attempt++) {
                int rc = RmGetList(h, out needed, ref count, info, ref reasons);
                if (rc == 0) {
                    for (int i = 0; i < count; i++) result.Add(info[i].Process.dwProcessId);
                    break;
                }
                if (rc != 234 /* ERROR_MORE_DATA: the list grew, size again */) break;
                info = new RM_PROCESS_INFO[needed + 2];
                count = (uint)info.Length;
            }
        } finally { RmEndSession(h); }
        return result.ToArray();
    }
}
}
'@
    }
    return @([BsE2E.FileHolders]::Get($Path))
}

# "pid N, <repo or command line>" for whoever holds the lock file, or '?' when unknown.
function Get-FileLockHolder([string]$Path) {
    $pids = @(@(try { Get-FileHolderPids $Path } catch { @() }) | Where-Object { $_ -ne $PID })
    $owner = try { (Get-Content "$Path.owner" -Raw -ErrorAction Stop).Trim() } catch { '' }
    if (-not $pids) {
        # Restart Manager could not tell (e.g. virtualized path): fall back to the owner file.
        if ($owner) { return "last recorded: $owner" } else { return '?' }
    }
    $h = $pids[0]
    if ($owner -match '^pid=(\d+) repo=(.*?) at=(\S+)' -and [int]$Matches[1] -eq $h) {
        return "pid $h, $($Matches[2]) (since $($Matches[3]))"
    }
    $cmd = try { (Get-CimInstance Win32_Process -Filter "ProcessId=$h" -ErrorAction Stop).CommandLine } catch { $null }
    if (-not $cmd) { $cmd = try { (Get-Process -Id $h -ErrorAction Stop).Path } catch { 'exited' } }
    return "pid $h, $cmd (another project's harness, e.g. BEI/BeetrootSkin)"
}

# Set when Enter-FileLock gave up, so callers can report "not run" instead of a failure.
$script:LockTimedOut = $false

# Waits for the lock, printing who holds it every -ReportSeconds. Throws after -TimeoutMinutes
# (0 or less = wait forever) and sets $script:LockTimedOut.
function Enter-FileLock([string]$Path, [int]$TimeoutMinutes = 120, [int]$PollSeconds = 5, [string]$What = 'lock', [int]$ReportSeconds = 60) {
    $script:LockTimedOut = $false
    $start = Get-Date
    $deadline = if ($TimeoutMinutes -gt 0) { $start.AddMinutes($TimeoutMinutes) } else { [datetime]::MaxValue }
    $nextReport = $start
    while ($true) {
        $s = Enter-FileLockOnce $Path
        if ($s) {
            $waited = [int]((Get-Date) - $start).TotalSeconds
            if ($waited -ge $PollSeconds) { Write-Step "got $What after $([Math]::Round($waited / 60.0, 1)) min" }
            return $s
        }
        $now = Get-Date
        if ($now -ge $nextReport) {
            $limit = if ($TimeoutMinutes -gt 0) { " of $TimeoutMinutes" } else { '' }
            Write-Step ("waiting for {0} ({1:N1}{2} min), held by {3} [{4}]" -f $What, ($now - $start).TotalMinutes, $limit, (Get-FileLockHolder $Path), $Path)
            $nextReport = $now.AddSeconds($ReportSeconds)
        }
        if ($now -gt $deadline) {
            $script:LockTimedOut = $true
            throw "Timed out after $TimeoutMinutes min waiting for $What $Path (held by $(Get-FileLockHolder $Path))"
        }
        Start-Sleep -Seconds $PollSeconds
    }
}

# Returns the open FileStream, or $null if somebody else holds the lock.
function Enter-FileLockOnce([string]$Path) {
    $dir = Split-Path $Path
    if (-not (Test-Path $dir)) { throw "lock directory $dir does not exist" }
    try {
        $s = [System.IO.File]::Open($Path, 'OpenOrCreate', 'ReadWrite', 'None')
    } catch [System.IO.DirectoryNotFoundException] { throw } catch [System.IO.IOException] { return $null } catch [System.UnauthorizedAccessException] { return $null }
    $info = "pid=$PID repo=$script:RepoRoot at=$(Get-Date -Format o)"
    $bytes = [Text.Encoding]::UTF8.GetBytes($info)
    $s.SetLength(0); $s.Write($bytes, 0, $bytes.Length); $s.Flush()
    try { Set-Content -Path "$Path.owner" -Value $info -ErrorAction Stop } catch {}
    return $s
}

function Exit-FileLock($Stream) { if ($Stream) { $Stream.Dispose() } }

# ---------------------------------------------------------------------------------------
# Selftest protocol (see README): the mod emits
#   SELFTEST PASS <name>
#   SELFTEST FAIL <name>: <reason>
#   SELFTEST DONE pass=<N> fail=<M>
# ---------------------------------------------------------------------------------------
$script:SelfTestRuns = [System.Collections.Generic.List[object]]::new()
$script:SelfTestDoneRegex = 'SELFTEST DONE pass=(\d+) fail=(\d+)'
# Command parser errors (e.g. unknown command, bad argument): stop waiting for DONE at once.
$script:CommandErrorRegex = 'Syntax error:|Unknown command|is not a valid command|You do not have permission'

# The command line for a selftest run. The glob is quoted because '*' and '.' are not valid in an
# unquoted command argument.
function Format-SelfTestCommand([string]$Glob) {
    if (-not $Glob) { return 'carryon selftest' }
    return "carryon selftest `"$($Glob -replace '"', '')`""
}

# Parse the selftest lines out of a block of log lines (the lines logged after the command was
# sent, up to and including the DONE line). The same line may appear twice (mod logger + command
# output); results are de-duplicated by name, and a FAIL wins over a PASS.
function ConvertFrom-SelfTestLines([string[]]$Lines, [string]$Glob) {
    $byName = [ordered]@{}
    $done = $null
    foreach ($l in $Lines) {
        if ($l -match 'SELFTEST PASS (\S+)\s*$') {
            if (-not $byName.Contains($Matches[1])) { $byName[$Matches[1]] = [pscustomobject]@{ Name = $Matches[1]; Status = 'PASS'; Reason = '' } }
        } elseif ($l -match 'SELFTEST FAIL ([^\s:]+):?\s*(.*?)\s*$') {
            $byName[$Matches[1]] = [pscustomobject]@{ Name = $Matches[1]; Status = 'FAIL'; Reason = $Matches[2] }
        } elseif ($l -match $script:SelfTestDoneRegex) {
            $done = [pscustomobject]@{ Pass = [int]$Matches[1]; Fail = [int]$Matches[2] }
        }
    }
    $results = @($byName.Values)
    $failed = @($results | Where-Object Status -eq 'FAIL')
    $passed = @($results | Where-Object Status -eq 'PASS')
    [pscustomobject]@{
        Glob    = $Glob
        Done    = [bool]$done
        Pass    = if ($done) { $done.Pass } else { $passed.Count }
        Fail    = if ($done) { $done.Fail } else { $failed.Count }
        Passed  = @($passed.Name)
        Failed  = $failed
        Results = $results
    }
}

# Record a selftest result, print a summary and return whether it is OK.
function Register-SelfTestResult($Result, [switch]$AllowEmpty) {
    $problems = @()
    if (-not $Result.Done) { $problems += 'no SELFTEST DONE line' }
    if ($Result.Fail -gt 0 -or $Result.Failed.Count -gt 0) { $problems += "$([Math]::Max($Result.Fail, $Result.Failed.Count)) failing test(s)" }
    if ($Result.Pass + $Result.Fail -eq 0 -and -not $AllowEmpty) { $problems += "no test matched '$($Result.Glob)'" }
    if ($Result.Done -and $Result.Results.Count -ne ($Result.Pass + $Result.Fail)) {
        Write-Step "warning: DONE says pass=$($Result.Pass) fail=$($Result.Fail) but $($Result.Results.Count) distinct result lines were parsed"
    }
    $Result | Add-Member -NotePropertyName Problems -NotePropertyValue $problems -Force
    $Result | Add-Member -NotePropertyName Ok -NotePropertyValue ($problems.Count -eq 0) -Force
    $script:SelfTestRuns.Add($Result)
    $color = if ($Result.Ok) { 'Green' } else { 'Red' }
    Write-Host "[e2e] selftest '$($Result.Glob)': pass=$($Result.Pass) fail=$($Result.Fail)" -ForegroundColor $color
    foreach ($f in $Result.Failed) { Write-Host "[e2e]   FAIL $($f.Name): $($f.Reason)" -ForegroundColor Red }
    foreach ($p in $problems) { if ($p -notmatch 'failing') { Write-Host "[e2e]   $p" -ForegroundColor Red } }
    return $Result.Ok
}

# Problems across every selftest run in this session (empty = all good).
function Get-SelfTestProblems {
    @($script:SelfTestRuns | Where-Object { -not $_.Ok } | ForEach-Object { "selftest '$($_.Glob)': $($_.Problems -join '; ')" })
}

function Save-SelfTestReport([string]$Path) {
    New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
    ConvertTo-Json -InputObject @($script:SelfTestRuns) -Depth 6 | Set-Content -Path $Path -Encoding UTF8
}

# Run tools/build.ps1 (owned by the mod build) for one target.
function Invoke-ModBuild([ValidateSet('server', 'client')] [string]$Target) {
    $build = Join-Path $script:RepoRoot 'tools/build.ps1'
    if (-not (Test-Path $build)) { throw "tools/build.ps1 not found; build the mod manually" }
    Write-Step "building ($Target)"
    & pwsh -NoProfile -File $build -Target $Target
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
}

# Regex for log lines that make a run fail: ERR/FTL from the mod, LeviLamina or CrashLogger.
# Matches both the server console format "12:00:00.000 ERR [Name] ..." and the client log
# file format "[2026-01-01 12:00:00.000 ERR][Name] ...".
$script:ErrorLineRegex = '\b(ERR|FTL)\]?\s?\[(LiftOff|LeviLamina|CrashLogger)\]'

# ---------------------------------------------------------------------------------------
# Shared scenario helpers. They rely on functions each platform's lib.ps1 defines:
#   Send-SelfTestCommand <cmd>, Get-LogMark, Get-ServerLog [-Since], Wait-LogLine
# ---------------------------------------------------------------------------------------

# Run 'carryon selftest "<glob>"' (no glob = all tests) and wait for SELFTEST DONE. Returns
# { Glob, Done, Pass, Fail, Passed[], Failed[{Name,Reason}], Results[], Ok, Problems[] }.
# The result is also recorded: run.ps1 fails the run if any selftest failed, did not finish or
# (unless -AllowEmpty) matched no test. It does not throw on test failures.
function Invoke-SelfTest([string]$Glob = '', [int]$TimeoutSeconds = 600, [switch]$AllowEmpty) {
    $mark = Get-LogMark
    Send-SelfTestCommand (Format-SelfTestCommand $Glob)
    $label = if ($Glob) { $Glob } else { '*' }
    try {
        $hit = Wait-LogLine "$script:SelfTestDoneRegex|$script:CommandErrorRegex" $TimeoutSeconds -Since $mark
        if ($hit -notmatch $script:SelfTestDoneRegex) { Write-Step "selftest command rejected: $hit" }
    } catch {
        Write-Step "selftest '$label' did not finish: $_"
    }
    $r = ConvertFrom-SelfTestLines (Get-ServerLog -Since $mark) $label
    Register-SelfTestResult $r -AllowEmpty:$AllowEmpty | Out-Null
    return $r
}

# Lines of this run that make it fail (ERR/FTL from the mod, LeviLamina or CrashLogger).
function Get-ErrorLines { @(Get-ServerLog | Where-Object { $_ -match $script:ErrorLineRegex }) }

# Config files written into the deployed mod before every run (`$Scenario` of run.ps1 is in
# scope). Use it to turn off features that would disturb the scenarios, e.g. world generation.
function Set-TestModConfig([string]$ModDest) {
    $dir = Join-Path $ModDest 'config'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}
