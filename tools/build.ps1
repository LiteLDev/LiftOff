# Builds LiftOff for the server (BDS), the client, or both.
#
#   pwsh tools/build.ps1                    # both platforms, release
#   pwsh tools/build.ps1 -Target server     # one platform
#   pwsh tools/build.ps1 -Tests             # also build and run the unit tests (lo-tests)
#
# Each platform has its own build dir (build/<target>); outputs land in
#   bin/<target>/LiftOff/  (dll, pdb, manifest.json, lang/, behavior_packs/, resource_packs/)
# Copy that folder to `plugins/` (BDS) or `mods/` (LeviLauncher client).
param(
    [ValidateSet('server', 'client', 'all')][string]$Target = 'all',
    [ValidateSet('release', 'debug')][string]$Mode = 'release',
    [switch]$Tests
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
Push-Location $root
try {
    # Fresh shells may not have the tools on PATH yet. Append (never prepend) the registry PATH so
    # the caller's PATH wins: on CI, setup-llvm puts LLVM 22 on PATH, and the runner's preinstalled
    # C:\Program Files\LLVM (listed in the machine PATH) must not shadow it.
    $env:Path = $env:Path + ';' + [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
    if ($env:LLVM_PATH -and (Test-Path (Join-Path $env:LLVM_PATH 'bin\clang-cl.exe'))) {
        $env:Path = (Join-Path $env:LLVM_PATH 'bin') + ";$env:Path"
    }
    if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
        foreach ($dir in "$env:ProgramFiles\LLVM\bin", "${env:ProgramFiles(x86)}\LLVM\bin") {
            if (Test-Path (Join-Path $dir 'clang-cl.exe')) { $env:Path = "$dir;$env:Path"; break }
        }
    }
    if (-not (Get-Command xmake -ErrorAction SilentlyContinue)) { throw 'xmake not found on PATH' }
    $clang = Get-Command clang-cl -ErrorAction SilentlyContinue
    if ($clang) { Write-Host "==> using $($clang.Source) ($((& $clang.Source --version | Select-Object -First 1)))" }
    git config --global core.longpaths true | Out-Null

    # xmake needs to be told about Visual Studio 18 (2026) explicitly.
    $vsArgs = @()
    foreach ($pf in $env:ProgramFiles, ${env:ProgramFiles(x86)}) {
        if ($pf -and (Test-Path (Join-Path $pf 'Microsoft Visual Studio\18'))) { $vsArgs = @('--vs=2026'); break }
    }

    $targets = if ($Target -eq 'all') { @('server', 'client') } else { @($Target) }
    foreach ($t in $targets) {
        Write-Host "==> configuring $t ($Mode)"
        xmake f -a x64 -m $Mode -p windows --target_type=$t -o "build/$t" @vsArgs -y
        if ($LASTEXITCODE) { throw "xmake config failed for $t" }
        Write-Host "==> building $t"
        xmake build -y LiftOff
        if ($LASTEXITCODE) { throw "build failed for $t" }
        $out = Join-Path $root "bin/$t/LiftOff"
        if (-not (Test-Path (Join-Path $out 'LiftOff.dll'))) { throw "missing output $out" }
        Write-Host "==> $t ready: $out"
    }
    if ($Tests) {
        Write-Host '==> unit tests'
        xmake build -y lo-tests
        if ($LASTEXITCODE) { throw 'lo-tests build failed' }
        xmake run lo-tests
        if ($LASTEXITCODE) { throw 'unit tests failed' }
    }
} finally {
    Pop-Location
}
