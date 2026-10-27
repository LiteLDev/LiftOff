<#
.SYNOPSIS
  Install (once per machine) Bedrock Dedicated Server + LeviLamina into the shared e2e cache.

.DESCRIPTION
  Installs github.com/LiteLDev/LeviLamina@<version> with lip into <home>\bds-base, where <home>
  is $env:LO_E2E_HOME or %LOCALAPPDATA%\lo-e2e. lip pulls the matching bedrock-server
  (github.com/LiteLDev/bds), CrashLogger, PreLoader, the runtime data, and patches
  bedrock_server.exe into bedrock_server_mod.exe.

  Idempotent and concurrency-safe: a run holds <home>\install.lock, installs into a temporary
  directory and renames it to bds-base only after it succeeded. bds-base\.lo-e2e-installed
  records what was installed. A second concurrent caller waits for the lock and then finds the
  installation complete.

.EXAMPLE
  pwsh tools/e2e/server/install.ps1            # no-op when already installed
  pwsh tools/e2e/server/install.ps1 -Force     # reinstall
#>
param(
    [string]$Version = '26.51.7',
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../common/common.ps1"

$home_ = Get-E2EHome
$base  = Join-Path $home_ 'bds-base'
$stampFile = Join-Path $base '.lo-e2e-installed'
$wanted = "github.com/LiteLDev/LeviLamina@$Version"

function Test-Installed {
    (Test-Path $stampFile) -and ((Get-Content $stampFile -Raw).Trim() -split "`n")[0].Trim() -eq $wanted -and
        (Test-Path (Join-Path $base 'bedrock_server_mod.exe'))
}

if (-not $Force -and (Test-Installed)) { Write-Step "BDS + LeviLamina $Version already installed at $base"; return }

if (-not (Get-Command lip -ErrorAction SilentlyContinue)) {
    throw "lip is not installed. Get it from https://github.com/LiteLDev/lip (npm install -g @levimc/lip, or 'winget install levimc.lip')"
}

$lock = Enter-FileLock (Join-Path $home_ 'install.lock') -TimeoutMinutes 60 -What 'install lock'
try {
    if (-not $Force -and (Test-Installed)) { Write-Step "installed by another process meanwhile"; return }

    $tmp = Join-Path $home_ "bds-base.tmp-$PID"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    # Leftovers of crashed installs.
    Get-ChildItem $home_ -Directory -Filter 'bds-base.tmp-*' -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $tmp | Out-Null

    Write-Step "installing $wanted into $tmp (downloads ~250 MB, takes a few minutes)"
    Push-Location $tmp
    try {
        # lip installs into the current directory; the LeviLamina tooth depends on
        # github.com/LiteLDev/bds (the server itself), CrashLogger, PreLoader, PeEditor, ...
        & lip install $wanted 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -notmatch '^Downloading|^\S*https?://\S*: \d+%$|^-server' } | ForEach-Object { Write-Host "  | $_" }
        if ($LASTEXITCODE -ne 0) { throw "lip install failed (exit $LASTEXITCODE)" }
    } finally { Pop-Location }

    if (-not (Test-Path (Join-Path $tmp 'bedrock_server_mod.exe'))) { throw "lip finished but bedrock_server_mod.exe is missing in $tmp" }
    # Mods live in plugins\ on LeviLamina 26.x servers (PreLoaderConfig modsPath); older ones use mods\.
    if (-not (Test-Path (Join-Path $tmp 'plugins\LeviLamina')) -and -not (Test-Path (Join-Path $tmp 'mods\LeviLamina'))) {
        throw "LeviLamina was not installed into $tmp"
    }
    # lip's download cache is not needed at runtime.
    Remove-Item -Recurse -Force (Join-Path $tmp '.cache') -ErrorAction SilentlyContinue

    Set-Content -Path (Join-Path $tmp '.lo-e2e-installed') -Value "$wanted`ninstalled=$(Get-Date -Format o)" -Encoding UTF8

    if (Test-Path $base) {
        $old = Join-Path $home_ "bds-base.old-$(Get-Date -Format yyyyMMddHHmmss)"
        Move-Item $base $old
        # Slots hold their own copies/hard links, so the old base can go (best effort).
        Remove-Item -Recurse -Force $old -ErrorAction SilentlyContinue
    }
    Move-Item $tmp $base
    Write-Step "installed BDS + LeviLamina $Version at $base"
} finally {
    Exit-FileLock $lock
}
