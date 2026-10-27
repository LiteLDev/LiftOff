# LiftOff client e2e helpers: deploy the mod into the LeviLauncher game instance,
# drive Minecraft Bedrock with input (the LeviHeadlessInput mod's control pipe by default, real
# SendInput with run.ps1 -Foreground) and capture screenshots.
# Adapted from BEI's tools/e2e/lib.ps1 (same machine, same game instance, same lock).
#
# Dot-sourced by run.ps1. Coordinates passed to Click/Move are client-area pixels of the game
# window, or fractions (0..1) of the client area with -Relative.

. "$PSScriptRoot/../common/common.ps1"

$script:ClientDir    = $PSScriptRoot
$script:ModName      = 'LiftOff'
$script:Launcher     = if ($env:LO_E2E_LAUNCHER) { $env:LO_E2E_LAUNCHER } else { Join-Path $env:USERPROFILE 'Desktop\LeviLauncher.exe' }
$script:LauncherData = Join-Path $env:APPDATA 'levilauncher.exe'
$script:BaseVersion  = if ($env:LO_E2E_CLIENT_VERSION) { $env:LO_E2E_CLIENT_VERSION } else { '1.26.51.01' }
# Mods that stay loaded during our runs. LeviHeadlessInput (headless input, see run.ps1) is
# installed fresh by every headless run and removed after it: never park it aside (a restored copy
# would make a normally started game ignore the real mouse and keyboard).
$script:KeepMods     = @('LeviLamina', 'CrashLogger', $script:ModName, 'LeviHeadlessInput')
$script:OutRoot      = Join-Path $PSScriptRoot 'out'
# LeviHeadlessInput (https://github.com/LiteLDev/LeviHeadlessInput, cloned and built in
# %USERPROFILE%\Desktop\LeviHeadlessInput by default): a test-only client
# mod that lets run.ps1 drive the game through a control pipe instead of the real mouse and
# keyboard. Its build output (DLL, HeadlessInput.psm1, harness-shim.ps1, ...) is taken from here.
$script:HeadlessInputDir = if ($env:LEVI_HEADLESS_INPUT_DIR) { $env:LEVI_HEADLESS_INPUT_DIR } else {
    Join-Path $env:USERPROFILE 'Desktop\LeviHeadlessInput\bin\LeviHeadlessInput' }

# Game instances ("slots"). Slot 1 is the LeviLauncher instance itself; its lock is shared with
# BEI's and BeetrootSkin's harnesses on purpose (they drive the same instance). Slots 2..N are
# clones made by LeviHeadlessInput's clone-instance.ps1 (versions\<base>-hi<N>: hard links + their
# own mods\, logs\ and game data) with a lock each, so headless runs can use several games at once.
# Foreground (real input) runs always use slot 1. Set-E2EInstance points every instance-specific
# path at a slot; run.ps1 picks the slot (Enter-E2ELock -Slots).
function Set-E2EInstance([int]$Slot) {
    $script:InstanceSlot = $Slot
    $script:VersionName  = if ($Slot -le 1) { $script:BaseVersion } else { "$($script:BaseVersion)-hi$Slot" }
    $script:VersionDir   = Join-Path $script:LauncherData "versions\$script:VersionName"
    $script:ModsDir      = Join-Path $script:VersionDir 'mods'
    # Other mods are moved here for the duration of a run (outside mods\, so LeviLamina ignores them).
    $script:AsideDir     = Join-Path $script:VersionDir 'mods.lo-e2e-aside'
    $script:LockFile     = if ($Slot -le 1) { Join-Path $script:LauncherData 'bei-e2e.lock' } else { Join-Path $script:LauncherData "hi$Slot-e2e.lock" }
    # LeviHeadlessInput control pipe of this instance's game.
    $script:HeadlessPipe = "LeviHeadlessInput-$script:VersionName"
    # Child pwsh processes that source this file (helpers, scenario scripts) start on the same slot.
    $env:LO_E2E_INSTANCE_SLOT = "$Slot"
}
Set-E2EInstance $(if ($env:LO_E2E_INSTANCE_SLOT -match '^\d+$') { [int]$env:LO_E2E_INSTANCE_SLOT } else { 1 })
$script:WorldName    = 'LO-Test'
$script:Unit         = 'scratch'

if (-not ('BsE2E.Native' -as [type])) {
    Add-Type -AssemblyName System.Drawing
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace BsE2E {
public static class Native {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] struct UNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public UNION u; }

    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    public static void ForceForeground(IntPtr h) {
        IntPtr fg = GetForegroundWindow();
        uint fgThread = GetWindowThreadProcessId(fg, IntPtr.Zero);
        uint me = GetCurrentThreadId();
        AttachThreadInput(me, fgThread, true);
        keybd_event(0x12, 0, 0, IntPtr.Zero); keybd_event(0x12, 0, 2, IntPtr.Zero);
        ShowWindow(h, 9);
        BringWindowToTop(h);
        SetForegroundWindow(h);
        AttachThreadInput(me, fgThread, false);
    }

    const uint INPUT_MOUSE = 0, INPUT_KEYBOARD = 1;
    const uint KEYEVENTF_KEYUP = 2, KEYEVENTF_UNICODE = 4, KEYEVENTF_EXTENDED = 1;
    const uint MOUSEEVENTF_MOVE = 1, MOUSEEVENTF_LEFTDOWN = 2, MOUSEEVENTF_LEFTUP = 4,
               MOUSEEVENTF_RIGHTDOWN = 8, MOUSEEVENTF_RIGHTUP = 16, MOUSEEVENTF_WHEEL = 0x800;

    static void Send(INPUT i) { SendInput(1, new[] { i }, Marshal.SizeOf(typeof(INPUT))); }

    // Set by LeviHeadlessInput's harness-shim.ps1 (Register-HeadlessNative): the input methods
    // below then go to the game through the mod's control pipe instead of SendInput.
    public static Action<ushort, bool> KeyRedirect;
    public static Action<int, bool> MouseButtonRedirect;
    public static Action<int> WheelRedirect;
    public static Action<char> UnicodeRedirect;
    public static Action<int, int> MoveRelativeRedirect;

    public static void Key(ushort vk, bool up) {
        if (KeyRedirect != null) { KeyRedirect(vk, up); return; }
        var i = new INPUT { type = INPUT_KEYBOARD };
        i.u.ki.wVk = vk;
        i.u.ki.wScan = (ushort)MapVirtualKey(vk, 0);
        i.u.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0);
        if (vk == 0x25 || vk == 0x26 || vk == 0x27 || vk == 0x28 || vk == 0x2E || vk == 0x24 || vk == 0x23) i.u.ki.dwFlags |= KEYEVENTF_EXTENDED;
        Send(i);
    }
    public static void Unicode(char c) {
        if (UnicodeRedirect != null) { UnicodeRedirect(c); return; }
        var down = new INPUT { type = INPUT_KEYBOARD }; down.u.ki.wScan = c; down.u.ki.dwFlags = KEYEVENTF_UNICODE;
        var up = new INPUT { type = INPUT_KEYBOARD }; up.u.ki.wScan = c; up.u.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        Send(down); Send(up);
    }
    public static void MouseButton(int button, bool up) {
        if (MouseButtonRedirect != null) { MouseButtonRedirect(button, up); return; }
        var i = new INPUT { type = INPUT_MOUSE };
        if (button == 0) i.u.mi.dwFlags = up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
        else i.u.mi.dwFlags = up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN;
        Send(i);
    }
    public static void Wheel(int delta) {
        if (WheelRedirect != null) { WheelRedirect(delta); return; }
        var i = new INPUT { type = INPUT_MOUSE }; i.u.mi.dwFlags = MOUSEEVENTF_WHEEL; i.u.mi.mouseData = (uint)delta; Send(i);
    }
    public static void MoveRelative(int dx, int dy) {
        if (MoveRelativeRedirect != null) { MoveRelativeRedirect(dx, dy); return; }
        var i = new INPUT { type = INPUT_MOUSE }; i.u.mi.dwFlags = MOUSEEVENTF_MOVE; i.u.mi.dx = dx; i.u.mi.dy = dy; Send(i);
    }
}
}
'@
}

# Physical pixels everywhere (the desktop uses display scaling).
[BsE2E.Native]::SetProcessDpiAwarenessContext([IntPtr]::new(-4)) | Out-Null

# ---------------------------------------------------------------------------------------
# Lock (one per game instance; slot 1's is shared with BEI's and BeetrootSkin's harnesses)
# ---------------------------------------------------------------------------------------
# Takes the lock of the first free slot in -Slots (default: the current one, see Set-E2EInstance)
# and switches every instance path to that slot.
function Enter-E2ELock([int]$TimeoutMinutes = 120, [int[]]$Slots = @($script:InstanceSlot)) {
    if ($Slots.Count -eq 1) {
        Set-E2EInstance $Slots[0]
        $script:LockStream = Enter-FileLock $script:LockFile -TimeoutMinutes $TimeoutMinutes -PollSeconds 2 -What 'game lock'
    } else {
        $script:LockTimedOut = $false
        $deadline = if ($TimeoutMinutes -gt 0) { (Get-Date).AddMinutes($TimeoutMinutes) } else { [datetime]::MaxValue }
        $nextReport = Get-Date
        while ($true) {
            foreach ($s in $Slots) {
                Set-E2EInstance $s
                $script:LockStream = Enter-FileLockOnce $script:LockFile
                if ($script:LockStream) { break }
            }
            if ($script:LockStream) { break }
            if ((Get-Date) -ge $nextReport) { Write-Step "waiting for a game lock (slots $($Slots -join ', '))"; $nextReport = (Get-Date).AddSeconds(60) }
            if ((Get-Date) -gt $deadline) { $script:LockTimedOut = $true; throw "Timed out after $TimeoutMinutes min waiting for a game lock (slots $($Slots -join ', '))" }
            Start-Sleep -Seconds 2
        }
    }
    Write-Step "game lock acquired (slot $script:InstanceSlot, instance $script:VersionName, $script:LockFile)"
}

function Exit-E2ELock {
    if ($script:LockStream) { Exit-FileLock $script:LockStream; $script:LockStream = $null; Write-Step "game lock released" }
}

# ---------------------------------------------------------------------------------------
# Deploy / other mods / world fixture
# ---------------------------------------------------------------------------------------
function Get-GameDataDir {
    # LeviLauncher isolation: per-version data under the version folder.
    $iso = Join-Path $script:VersionDir 'Minecraft Bedrock'
    if (Test-Path $iso) { return $iso }
    return Join-Path $env:APPDATA 'Minecraft Bedrock'
}

# Puts the camera back to first person (options.txt `game_thirdperson:0`) so a scenario that
# switched views (F5 / camera presets) can't leave the shared game in third person for later runs.
function Reset-CameraPerspective {
    $data = Get-GameDataDir
    foreach ($o in Get-ChildItem -Recurse -Path $data -Filter 'options.txt' -ErrorAction SilentlyContinue) {
        $lines = Get-Content $o.FullName
        if ($lines -match '^game_thirdperson:[1-9]') {
            $lines -replace '^game_thirdperson:\d+', 'game_thirdperson:0' | Set-Content $o.FullName
            Write-Step "reset camera perspective to first person in $($o.FullName)"
        }
    }
}

function Get-WorldsDir {
    $data = Get-GameDataDir
    $dirs = @(Get-ChildItem (Join-Path $data 'Users') -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'games\com.mojang\minecraftWorlds' } |
        Where-Object { Test-Path $_ })
    # Prefer a real user folder over "Shared".
    $user = $dirs | Where-Object { $_ -notmatch '\\Shared\\' } | Select-Object -First 1
    if ($user) { return $user }
    return $dirs | Select-Object -First 1
}

function Deploy-Mod([string]$BinDir) {
    if (-not (Test-Path (Join-Path $BinDir "$script:ModName.dll"))) { throw "Build output not found: $BinDir (run 'pwsh tools/build.ps1 -Target client' or pass -Build)" }
    $dest = Join-Path $script:ModsDir $script:ModName
    # Fresh copy every run (no config/data carried over), so runs are reproducible.
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Copy-Item -Recurse -Force $BinDir $dest
    Set-TestModConfig $dest
    Write-Step "deployed $BinDir -> $dest"
}

function Remove-DeployedMod {
    $dest = Join-Path $script:ModsDir $script:ModName
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue; Write-Step "removed $dest" }
}

# Remove mods\LeviHeadlessInput (a leftover of a killed run, or this run's copy once the game is
# closed), retrying while the game still holds the DLL open.
function Remove-HeadlessInputLeftover {
    $dest = Join-Path $script:ModsDir 'LeviHeadlessInput'
    for ($i = 0; $i -lt 10 -and (Test-Path $dest); $i++) {
        try { Remove-Item -Recurse -Force $dest -ErrorAction Stop } catch { Start-Sleep -Seconds 2 }
    }
    if (Test-Path $dest) { Write-Host "[e2e] FAILED to remove $dest; delete it by hand" -ForegroundColor Red }
}

# Move every mod except LeviLamina/CrashLogger/ours (e.g. BEI) out of mods\ so it cannot draw on
# or react to our screens. Restore-OtherMods puts them back; it also runs at the start of each
# run, so a run that was killed half-way is repaired by the next one.
function Move-OtherModsAside {
    New-Item -ItemType Directory -Force $script:AsideDir | Out-Null
    foreach ($m in Get-ChildItem $script:ModsDir -Directory) {
        if ($m.Name -in $script:KeepMods) { continue }
        Move-Item $m.FullName (Join-Path $script:AsideDir $m.Name)
        Write-Step "moved mod '$($m.Name)' aside"
    }
}

function Restore-OtherMods {
    if (-not (Test-Path $script:AsideDir)) { return }
    foreach ($m in Get-ChildItem $script:AsideDir -Directory | Where-Object Name -ne '.stale') {
        $dst = Join-Path $script:ModsDir $m.Name
        if (Test-Path $dst) {
            # Somebody (e.g. BEI's harness) re-deployed the mod while our copy was aside: the
            # aside copy is stale. Keep it out of the way instead of deleting it.
            $stale = Join-Path $script:AsideDir ".stale\$($m.Name)-$(Get-Date -Format yyyyMMddHHmmss)"
            New-Item -ItemType Directory -Force (Split-Path $stale) | Out-Null
            Move-Item $m.FullName $stale
            Write-Step "warning: mods\$($m.Name) was re-created meanwhile; moved the old aside copy to $stale"
            continue
        }
        Move-Item $m.FullName $dst
        Write-Step "restored mod '$($m.Name)'"
    }
    if (-not (Get-ChildItem $script:AsideDir -Force)) { Remove-Item $script:AsideDir -Force }
}

# Install the LO-Test world from fixtures\LO-Test (a level.dat template; the game generates the
# terrain on first load). LastPlayed is set to now so it is the first world in the list.
function Restore-TestWorld {
    . "$PSScriptRoot/../common/nbt.ps1"
    $src = Join-Path $PSScriptRoot "fixtures\$script:WorldName"
    $worlds = Get-WorldsDir
    if (-not $worlds) { throw "worlds dir not found under $(Get-GameDataDir) (start the game once by hand and sign in)" }
    Remove-TestWorld
    $target = Join-Path $worlds $script:WorldName
    if (Test-Path $target) { throw "could not delete the previous $target (is a file still open?)" }
    Copy-Item -Recurse $src $target
    $ld = Read-LevelDat (Join-Path $target 'level.dat')
    Set-NbtValue $ld.Root 'LastPlayed' 4 ([long][DateTimeOffset]::UtcNow.ToUnixTimeSeconds())
    Write-LevelDat $ld (Join-Path $target 'level.dat')
    Write-Step "restored world fixture to $target"
}

# Delete every world called LO-Test. Run after each run so that it never shows up as the most
# recently played world in BEI's harness (which opens the first world in the list).
function Remove-TestWorld {
    $worlds = Get-WorldsDir
    if (-not $worlds) { return }
    Get-ChildItem $worlds -Directory | Where-Object {
        $_.Name -eq $script:WorldName -or (Get-Content (Join-Path $_.FullName 'levelname.txt') -ErrorAction SilentlyContinue) -eq $script:WorldName
    } | ForEach-Object { Remove-Item -Recurse -Force $_.FullName -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------------------
# Game process / window
# ---------------------------------------------------------------------------------------
# The games of this slot's instance only (other slots may run games of their own): matched by the
# exe's folder, since every instance runs a Minecraft.Windows.exe.
function Get-InstanceProcesses {
    $dir = $script:VersionDir.TrimEnd('\') + '\'
    Get-Process -Name 'Minecraft.Windows' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($dir, [StringComparison]::OrdinalIgnoreCase) }
}
function Get-GameProcess { Get-InstanceProcesses | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1 }

function Start-Game([int]$TimeoutSeconds = 180) {
    if (Get-GameProcess) { throw "Minecraft is already running; close it first" }
    # latest.log is appended across sessions: remember where this session starts.
    Start-LogSession
    if ($script:Headless) {
        # Straight from the instance folder (the exe loads PreLoader/LeviLamina itself), with a
        # window that is shown without being activated; no launcher window either.
        Write-Step "launching $script:VersionName in the background"
        & (Join-Path $script:HeadlessInputDir 'Start-HeadlessGame.ps1') -Instance $script:VersionName | Out-Null
    } else {
        if (-not (Test-Path $script:Launcher)) { throw "LeviLauncher not found at $script:Launcher (set `$env:LO_E2E_LAUNCHER)" }
        # A leftover launcher instance swallows --launch (single-instance hand-off) without
        # starting the game, so close any running launcher first.
        Get-Process -Name 'LeviLauncher' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
        Write-Step "launching $script:VersionName via LeviLauncher"
        Start-Process -FilePath $script:Launcher -ArgumentList "--launch=`"$script:VersionName`""
    }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $p = Get-GameProcess
        if ($p) { $script:Game = $p; Write-Step "game window found (pid $($p.Id))"; return $p }
        Start-Sleep -Seconds 2
    }
    throw "Game window did not appear within $TimeoutSeconds s"
}

function Stop-Game {
    Get-InstanceProcesses | ForEach-Object {
        $_.CloseMainWindow() | Out-Null
        if (-not $_.WaitForExit(10000)) { $_.Kill() }
    }
    if (-not $script:Headless) { Get-Process -Name 'LeviLauncher' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
    Write-Step "game stopped"
}

function Get-GameWindow {
    $p = Get-GameProcess
    if (-not $p) { throw "Minecraft is not running" }
    return $p.MainWindowHandle
}

# Fixed window size so GUI scale and click coordinates are reproducible across runs.
$script:WindowW = 1600
$script:WindowH = 900

function Focus-Game {
    $h = Get-GameWindow
    if ([BsE2E.Native]::GetForegroundWindow() -eq $h) { return }
    # Keep the game above other windows so synthesized clicks land on it, then click its
    # title bar: a (synthesized) user click is allowed to change the foreground window even
    # when SetForegroundWindow is blocked by the focus-stealing rules.
    [BsE2E.Native]::SetWindowPos($h, [IntPtr]::new(-1), 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0040) | Out-Null
    $wr = New-Object BsE2E.Native+RECT; [BsE2E.Native]::GetWindowRect($h, [ref]$wr) | Out-Null
    $c = Get-ClientRect
    if ($c.Y - $wr.Top -gt 4) {
        [BsE2E.Native]::SetCursorPos([int](($wr.Left + $wr.Right) / 2), [int](($wr.Top + $c.Y) / 2)) | Out-Null
        [BsE2E.Native]::MouseButton(0, $false); Start-Sleep -Milliseconds 50; [BsE2E.Native]::MouseButton(0, $true)
        Start-Sleep -Milliseconds 300
    }
    for ($i = 0; $i -lt 5; $i++) {
        [BsE2E.Native]::ForceForeground($h)
        Start-Sleep -Milliseconds 300
        if ([BsE2E.Native]::GetForegroundWindow() -eq $h) { return }
    }
    Write-Step "warning: could not bring the game window to the foreground"
}

function Set-GameWindowSize([int]$ClientW = $script:WindowW, [int]$ClientH = $script:WindowH) {
    $h = Get-GameWindow
    [BsE2E.Native]::ShowWindow($h, 9) | Out-Null  # restore (un-maximize)
    $wr = New-Object BsE2E.Native+RECT; [BsE2E.Native]::GetWindowRect($h, [ref]$wr) | Out-Null
    $c = Get-ClientRect
    $extraW = ($wr.Right - $wr.Left) - $c.W; $extraH = ($wr.Bottom - $wr.Top) - $c.H
    [BsE2E.Native]::SetWindowPos($h, [IntPtr]::Zero, 40, 40, $ClientW + $extraW, $ClientH + $extraH, 0x0040) | Out-Null
    Start-Sleep -Milliseconds 800
}

function Get-ClientRect {
    $h = Get-GameWindow
    $r = New-Object BsE2E.Native+RECT
    [BsE2E.Native]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = New-Object BsE2E.Native+POINT
    [BsE2E.Native]::ClientToScreen($h, [ref]$pt) | Out-Null
    [pscustomobject]@{ X = $pt.X; Y = $pt.Y; W = $r.Right - $r.Left; H = $r.Bottom - $r.Top }
}

function ConvertTo-Screen([double]$x, [double]$y, [switch]$Relative) {
    $c = Get-ClientRect
    if ($Relative) { $x = $x * $c.W; $y = $y * $c.H }
    [pscustomobject]@{ X = [int]($c.X + $x); Y = [int]($c.Y + $y) }
}

# ---------------------------------------------------------------------------------------
# Input
# ---------------------------------------------------------------------------------------
# Safety: never send synthesized input unless Minecraft is the foreground window, so keys and
# clicks can never land in another application.
function Assert-GameForeground {
    $h = Get-GameWindow
    if ([BsE2E.Native]::GetForegroundWindow() -ne $h) {
        Focus-Game
        if ([BsE2E.Native]::GetForegroundWindow() -ne $h) { throw "Minecraft is not the foreground window; refusing to send input (a UAC/firewall dialog may be in front)" }
    }
}

function Move-Mouse([double]$x, [double]$y, [switch]$Relative) {
    Assert-GameForeground
    $p = ConvertTo-Screen $x $y -Relative:$Relative
    [BsE2E.Native]::SetCursorPos($p.X, $p.Y) | Out-Null
    # A tiny relative move makes the game register the new position.
    [BsE2E.Native]::MoveRelative(1, 0); [BsE2E.Native]::MoveRelative(-1, 0)
    Start-Sleep -Milliseconds 120
}

function Click([double]$x, [double]$y, [switch]$Relative, [switch]$Right) {
    Move-Mouse $x $y -Relative:$Relative
    $b = if ($Right) { 1 } else { 0 }
    [BsE2E.Native]::MouseButton($b, $false); Start-Sleep -Milliseconds 60; [BsE2E.Native]::MouseButton($b, $true)
    Start-Sleep -Milliseconds 250
}

function Scroll([int]$notches) { Assert-GameForeground; [BsE2E.Native]::Wheel(120 * $notches); Start-Sleep -Milliseconds 200 }

$script:VK = @{
    ESC = 0x1B; ENTER = 0x0D; BACK = 0x08; TAB = 0x09; SPACE = 0x20; SHIFT = 0x10; CTRL = 0x11
    LEFT = 0x25; UP = 0x26; RIGHT = 0x27; DOWN = 0x28; F1 = 0x70; F2 = 0x71; F3 = 0x72
}

function Press-Key([string]$key, [int]$holdMs = 60) {
    Assert-GameForeground
    $vk = if ($script:VK.ContainsKey($key.ToUpper())) { $script:VK[$key.ToUpper()] } else { [int][char]$key.ToUpper() }
    [BsE2E.Native]::Key([uint16]$vk, $false); Start-Sleep -Milliseconds $holdMs; [BsE2E.Native]::Key([uint16]$vk, $true)
    Start-Sleep -Milliseconds 200
}

function Type-Text([string]$text) {
    Assert-GameForeground
    foreach ($c in $text.ToCharArray()) { [BsE2E.Native]::Unicode($c); Start-Sleep -Milliseconds 40 }
    Start-Sleep -Milliseconds 200
}

# ---------------------------------------------------------------------------------------
# Screenshots and logs
# ---------------------------------------------------------------------------------------
# Saves out\<unit>\<name>.png (unit defaults to the running scenario), downscaled to 1280 wide.
function Save-Screenshot([string]$name, [string]$unit = $script:Unit) {
    $c = Get-ClientRect
    $dir = Join-Path $script:OutRoot $unit
    New-Item -ItemType Directory -Force $dir | Out-Null
    # Capture the window itself (works even when covered by other windows), then crop to
    # the client area.
    $h = Get-GameWindow
    $wr = New-Object BsE2E.Native+RECT; [BsE2E.Native]::GetWindowRect($h, [ref]$wr) | Out-Null
    $full = New-Object System.Drawing.Bitmap ($wr.Right - $wr.Left), ($wr.Bottom - $wr.Top)
    $gf = [System.Drawing.Graphics]::FromImage($full)
    $hdc = $gf.GetHdc()
    [BsE2E.Native]::PrintWindow($h, $hdc, 2) | Out-Null   # PW_RENDERFULLCONTENT
    $gf.ReleaseHdc($hdc); $gf.Dispose()
    $bmp = New-Object System.Drawing.Bitmap $c.W, $c.H
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.DrawImage($full, (New-Object System.Drawing.Rectangle 0, 0, $c.W, $c.H), (New-Object System.Drawing.Rectangle ($c.X - $wr.Left), ($c.Y - $wr.Top), $c.W, $c.H), 'Pixel')
    $full.Dispose()
    $scale = [Math]::Min(1.0, 1280.0 / $c.W)
    $out = New-Object System.Drawing.Bitmap ([int]($c.W * $scale)), ([int]($c.H * $scale))
    $g2 = [System.Drawing.Graphics]::FromImage($out)
    $g2.InterpolationMode = 'HighQualityBicubic'
    $g2.DrawImage($bmp, 0, 0, $out.Width, $out.Height)
    $path = Join-Path $dir "$name.png"
    $out.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $g2.Dispose(); $bmp.Dispose(); $out.Dispose()
    Write-Step "screenshot $path"
    return $path
}

# Session log: LeviLamina appends logs\latest.log across game sessions. Start-LogSession (called
# by Start-Game) remembers the current end of the file; Get-LeviLog then reads only what was
# appended since, incrementally (byte offset), into $script:LogLines.
$script:LogFile    = Join-Path $script:VersionDir 'logs\latest.log'
$script:LogLines   = [System.Collections.Generic.List[string]]::new()
$script:LogPos     = 0L
$script:LogTail    = ''
$script:LogDecoder = [Text.Encoding]::UTF8.GetDecoder()

function Start-LogSession {
    $script:LogFile = Join-Path $script:VersionDir 'logs\latest.log'   # this slot's instance
    $script:LogLines.Clear(); $script:LogTail = ''; $script:LogDecoder = [Text.Encoding]::UTF8.GetDecoder()
    $script:LogPos = if (Test-Path $script:LogFile) { (Get-Item $script:LogFile).Length } else { 0L }
}

# Remote modes: the BDS console log (tools/e2e/server/out/client-host-<unit>/server.log) is merged into
# the session log (lines prefixed "[host] "), so scenarios wait for server-side lines the same
# way in every mode.
$script:HostLog    = $null
$script:HostLogPos = 0L
$script:HostTail   = ''

function Update-HostLog {
    if (-not $script:HostLog -or -not (Test-Path $script:HostLog)) { return }
    $fs = [System.IO.File]::Open($script:HostLog, 'Open', 'Read', 'ReadWrite')
    try {
        if ($fs.Length -lt $script:HostLogPos) { $script:HostLogPos = 0L }
        if ($fs.Length -eq $script:HostLogPos) { return }
        $fs.Position = $script:HostLogPos
        $buf = New-Object byte[] ($fs.Length - $script:HostLogPos)
        $n = $fs.Read($buf, 0, $buf.Length)
        $script:HostLogPos += $n
    } finally { $fs.Dispose() }
    $parts = ($script:HostTail + [Text.Encoding]::UTF8.GetString($buf, 0, $n)) -split "`n"
    $script:HostTail = $parts[-1]
    for ($i = 0; $i -lt $parts.Count - 1; $i++) {
        $l = $parts[$i].TrimEnd("`r")
        if ($l -ne '') { $script:LogLines.Add("[host] $l") }
    }
}

function Update-LeviLog {
    Update-HostLog
    if (-not (Test-Path $script:LogFile)) { return }
    # The game keeps the file open for writing: read with FileShare.ReadWrite.
    $fs = [System.IO.File]::Open($script:LogFile, 'Open', 'Read', 'ReadWrite')
    try {
        # A shorter file means LeviLamina started a new one (rotation): read it from the start.
        if ($fs.Length -lt $script:LogPos) { $script:LogPos = 0L; $script:LogDecoder = [Text.Encoding]::UTF8.GetDecoder() }
        if ($fs.Length -eq $script:LogPos) { return }
        $fs.Position = $script:LogPos
        $buf = New-Object byte[] ($fs.Length - $script:LogPos)
        $n = $fs.Read($buf, 0, $buf.Length)
        $script:LogPos += $n
    } finally { $fs.Dispose() }
    $chars = New-Object char[] ($script:LogDecoder.GetCharCount($buf, 0, $n))
    $script:LogDecoder.GetChars($buf, 0, $n, $chars, 0) | Out-Null
    $parts = ($script:LogTail + [string]::new($chars)) -split "`n"
    $script:LogTail = $parts[-1]
    for ($i = 0; $i -lt $parts.Count - 1; $i++) {
        $l = $parts[$i].TrimEnd("`r")
        if ($l -ne '') { $script:LogLines.Add($l) }
    }
}

# Lines of latest.log written by the current game session (-Since <mark>: only lines after
# Get-LogMark returned <mark>; -All: the whole file, all sessions).
function Get-LeviLog([switch]$All, [int]$Since = 0) {
    if ($All) {
        if (-not (Test-Path $script:LogFile)) { return @() }
        $fs = [System.IO.File]::Open($script:LogFile, 'Open', 'Read', 'ReadWrite')
        try { $text = (New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $fs.Dispose() }
        return @($text -split "`r?`n" | Where-Object { $_ -ne '' })
    }
    Update-LeviLog
    if ($Since -ge $script:LogLines.Count) { return @() }
    return $script:LogLines.GetRange($Since, $script:LogLines.Count - $Since).ToArray()
}

# Same name as the server helper, so scenarios read alike.
function Get-ServerLog([int]$Since = 0) { Get-LeviLog -Since $Since }

# Current position in this session's log; pass it to Wait-LogLine/Get-LeviLog -Since.
function Get-LogMark { Update-LeviLog; $script:LogLines.Count }

# Wait for a line matching the regex in this session's log (from -Since). Returns the last match.
function Wait-LogLine([string]$pattern, [int]$TimeoutSeconds = 120, [int]$Since = 0) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ($true) {
        $hit = Get-LeviLog -Since $Since | Select-String -Pattern $pattern | Select-Object -Last 1
        if ($hit) { return $hit.Line }
        if (-not (Get-GameProcess)) { throw "Game exited while waiting for log '$pattern'" }
        if ((Get-Date) -gt $deadline) { throw "Timed out after $TimeoutSeconds s waiting for log line '$pattern'" }
        Start-Sleep -Seconds 1
    }
}

# Used by Invoke-SelfTest (common.ps1): commands are typed into chat.
function Send-SelfTestCommand([string]$Command) { Invoke-Chat "/$Command" }
function Assert-NoErrors {
    $bad = Get-ErrorLines
    if ($bad) { $bad | Select-Object -First 20 | ForEach-Object { Write-Host "  ! $_" -ForegroundColor Red }; throw "$($bad.Count) ERR/FTL line(s) from $script:ModName/LeviLamina/CrashLogger in the log" }
    Write-Step "no errors in log"
}

# ---------------------------------------------------------------------------------------
# Menu navigation (calibrated for a 1600x900 client area; coordinates are fractions)
# ---------------------------------------------------------------------------------------
function Wait-GameReady([int]$TimeoutSeconds = 180, [switch]$WithoutMod) {
    Set-GameWindowSize
    if ($WithoutMod) {
        # With no mods at all LeviLamina logs "0 mod(s) loaded" and never "enabled"; that early line
        # is sometimes missed after a game restart, so a running game counts as ready after 90 s.
        try {
            Wait-LogLine '\[LeviLamina\] (\d+ mod\(s\) enabled|0 mod\(s\) loaded)' ([Math]::Min(90, $TimeoutSeconds)) | Out-Null
        } catch {
            if (-not (Get-GameProcess)) { throw }
            Write-Step "no LeviLamina mod line in the log; the game is running, continuing"
        }
    } else {
        $hit = Wait-LogLine "$script:ModName enabled|Failed to (load|enable) mod $script:ModName" $TimeoutSeconds
        if ($hit -notmatch "$script:ModName enabled") { throw "LeviLamina failed to load $($script:ModName): $hit" }
    }
    # The title screen takes a while after mods are enabled.
    Start-Sleep -Seconds 15
    Focus-Game
    # First-launch sign-in nag: "Maybe later" (harmless click if absent: empty area).
    Click 0.781 0.775 -Relative
    Start-Sleep -Seconds 2
}

function Enter-TestWorld([int]$LoadSeconds = 10, [int]$Attempts = 3) {
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        Focus-Game
        $mark = Get-LogMark
        Click 0.498 0.474 -Relative     # Play
        Start-Sleep -Seconds 4
        Click 0.173 0.43 -Relative      # first world thumbnail (LO-Test: LastPlayed = now)
        Start-Sleep -Seconds $LoadSeconds
        try {
            Wait-LogLine 'Player Spawned' 75 -Since $mark | Out-Null
            Start-Sleep -Seconds 3
            Focus-Game
            Write-Step "in world"
            return
        } catch {
            if ($attempt -eq $Attempts) { throw }
            # The game sometimes fails to join its own local world ("Connection timed out").
            # Leave the error dialog and try again.
            Write-Step "world join failed (attempt $attempt): $_; retrying"
            Save-Screenshot "join-failed-$attempt" | Out-Null
            Focus-Game
            Click 0.24 0.68 -Relative   # "Back to menu" on the connection error dialog
            Start-Sleep -Seconds 5
        }
    }
}

# ---------------------------------------------------------------------------------------
# Remote modes: join a BDS (tools/e2e/server, scenario client-host) instead of the local world
# ---------------------------------------------------------------------------------------
$script:ExternalServerName = 'LO-E2E'

# The external server list of the signed-in user (games\com.mojang\minecraftpe\external_servers.txt).
function Get-ExternalServersFile {
    $worlds = Get-WorldsDir
    if (-not $worlds) { throw "game data not found under $(Get-GameDataDir)" }
    return Join-Path (Split-Path $worlds) 'minecraftpe\external_servers.txt' # minecraftWorlds -> com.mojang
}

# Backs up the game's server list and drops stale entries of ours; Enter-RemoteServer adds the
# server through the game's "Add server" dialog, Restore-ExternalServers puts the list back.
function Set-ExternalServer([int]$Port) {
    $file = Get-ExternalServersFile
    New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null
    $script:ExternalServersBackup = if (Test-Path $file) { Get-Content $file -Raw } else { $null }
    Clear-OurServerEntries
    $script:ExternalServerPort = $Port
    Write-Step "external server $script:ExternalServerName -> 127.0.0.1:$Port (added in game; list backed up from $file)"
}

# Drops our entries from the game's server list (the game must not be running: it rewrites the
# file from memory when it exits).
function Clear-OurServerEntries {
    $file = Get-ExternalServersFile
    if (-not (Test-Path $file)) { return }
    $content = Get-Content $file -Raw
    $keep    = @(Remove-OurServerEntries $content)
    $removed = @($content -split "`r?`n" | Where-Object { $_ }).Count - $keep.Count
    if ($removed -le 0) { return }
    if ($keep.Count -eq 0) { Remove-Item $file -Force } else { Set-Content -Path $file -Value $keep -Encoding UTF8 }
    Write-Step "removed $removed stale $script:ExternalServerName server entr$(if ($removed -eq 1) { 'y' } else { 'ies' }) from $file"
}

# The list without our entries (also ones an interrupted run left behind).
function Remove-OurServerEntries([string]$content) {
    if (-not $content) { return @() }
    return @($content -split "`r?`n" | Where-Object { $_ -and $_ -notmatch ":$([regex]::Escape($script:ExternalServerName)):" })
}

function Restore-ExternalServers {
    if (-not (Get-Variable -Name ExternalServersBackup -Scope Script -ErrorAction SilentlyContinue)) { return }
    $file = Get-ExternalServersFile
    $keep = @(Remove-OurServerEntries $script:ExternalServersBackup)
    if ($keep.Count -eq 0) { Remove-Item $file -ErrorAction SilentlyContinue }
    else { Set-Content -Path $file -Value $keep -Encoding UTF8 }
    Remove-Variable -Name ExternalServersBackup -Scope Script
}

# Play -> Servers -> "+ Add server" -> name / 127.0.0.1 / port -> "Add and play"; waits for the
# BDS to log the spawn. (User-added servers are listed below the featured ones, out of view, so the
# dialog is the deterministic path; the entry it adds is undone by Restore-ExternalServers.)
# Coordinates are fractions of the client area (calibrated on 1.26.51 at 1280x720 and 1600x900).
function Enter-RemoteServer([int]$Attempts = 3) {
    if (-not $script:ExternalServerPort) { throw 'Enter-RemoteServer needs Set-ExternalServer first' }
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        Focus-Game
        $mark = Get-LogMark
        Click 0.498 0.474 -Relative      # Play
        Start-Sleep -Seconds 4
        Save-Screenshot "join-1-play-$attempt" | Out-Null
        Click $script:ServersTab[0] $script:ServersTab[1] -Relative
        Start-Sleep -Seconds 3
        Save-Screenshot "join-2-servers-$attempt" | Out-Null
        Click $script:AddServer[0] $script:AddServer[1] -Relative
        Start-Sleep -Seconds 2
        # The dialog keeps the text of an earlier attempt: clear each field before typing.
        $fill = {
            param($at, [string]$text)
            Click $at[0] $at[1] -Relative
            for ($i = 0; $i -lt 24; $i++) { Press-Key BACK 20 }
            Type-Text $text
        }
        & $fill $script:NameField $script:ExternalServerName
        # The dialog refuses an address/port that is already listed ("already been added"):
        # Clear-OurServerEntries runs before every game launch, and run.ps1 joins once per launch.
        & $fill $script:AddressField '127.0.0.1'
        & $fill $script:PortField "$script:ExternalServerPort"
        Start-Sleep -Milliseconds 500
        Save-Screenshot "join-3-add-$attempt" | Out-Null
        Click $script:AddAndPlay[0] $script:AddAndPlay[1] -Relative
        try {
            Wait-LogLine '\[host\].*Player Spawned' 90 -Since $mark | Out-Null
            Start-Sleep -Seconds 4
            Focus-Game
            Write-Step "joined the remote server"
            return
        } catch {
            if ($attempt -eq $Attempts) { throw }
            Write-Step "remote join failed (attempt $attempt): $_; retrying"
            Save-Screenshot "join-failed-$attempt" | Out-Null
            # Back to the title screen: connecting dialog / Play screen (never a third ESC: exit dialog).
            Press-Key ESC; Start-Sleep -Seconds 3
            Save-Screenshot "join-reset-$attempt" | Out-Null
        }
    }
}
$script:ServersTab   = @(0.832, 0.13)   # "Servers" tab on the Play screen
$script:AddServer    = @(0.176, 0.253)  # "+ Add server" at the top of the server list
$script:NameField    = @(0.5, 0.20)     # "Add a new server" dialog fields
$script:AddressField = @(0.5, 0.366)
$script:PortField    = @(0.5, 0.532)
$script:AddAndPlay   = @(0.693, 0.663)  # "Add and play"

# Runs a console command on the remote host (remote modes).
function Send-HostConsole([string]$Command) {
    if (-not $script:HostDir) { throw 'Send-HostConsole needs a remote mode' }
    $name = 'cmd-{0:D6}.txt' -f (++$script:HostCmd)
    # Written under another name and renamed, so the host never reads a half-written file.
    $tmp = Join-Path $script:HostDir "$name.tmp"
    Set-Content -Path $tmp -Value $Command -Encoding UTF8
    Move-Item -Force $tmp (Join-Path $script:HostDir $name)
}
$script:HostCmd = 0

# Open the survival inventory (E) and wait for it to render.
function Open-Inventory { Focus-Game; Press-Key E; Start-Sleep -Milliseconds 1200 }
function Close-Screen { Press-Key ESC; Start-Sleep -Milliseconds 800 }

# Run a chat command, e.g. Invoke-Chat '/give @s diamond 16'
function Invoke-Chat([string]$text) {
    Focus-Game
    Press-Key T; Start-Sleep -Milliseconds 700
    Type-Text $text
    Press-Key ENTER; Start-Sleep -Milliseconds 800
}
