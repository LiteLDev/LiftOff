# Host for the client harness's remote modes (tools/e2e/client/run.ps1 -Mode remote-mod |
# remote-vanilla). Not a test on its own: it keeps the server running for a real game client.
#
# Contract with the client harness, through the folder in $env:LO_E2E_HOST_DIR:
#   ready.json      written once the server is up: {"port": <udp port>, "slot": n}
#                   (written to a temporary name and renamed, so it is never seen half-written)
#   cmd-*.txt       a console command to run (processed in name order, then deleted)
#   stop            the client is done: stop the server
# and $env:LO_E2E_HOST_OWNER_PID, the client run: the host stops as soon as it is gone (killed
# without running its cleanup), so the slot is not held for nothing.
$hostDir = $env:LO_E2E_HOST_DIR
if (-not $hostDir) { throw 'client-host needs $env:LO_E2E_HOST_DIR' }
New-Item -ItemType Directory -Force $hostDir | Out-Null
$owner = $null
$ownerGone = $false
if ($env:LO_E2E_HOST_OWNER_PID) {
    # Reading .Handle keeps a process handle open, so a reused pid cannot be mistaken for it.
    try { $owner = Get-Process -Id ([int]$env:LO_E2E_HOST_OWNER_PID) -ErrorAction Stop; $null = $owner.Handle } catch { $ownerGone = $true }
}
if ($ownerGone) {
    Write-Step "the client run (pid $env:LO_E2E_HOST_OWNER_PID) is already gone; not hosting"
} else {
    $tmp = Join-Path $hostDir 'ready.json.tmp'
    @{ port = $script:ServerPort; slot = $script:Slot } | ConvertTo-Json | Set-Content $tmp -Encoding UTF8
    Move-Item -Force $tmp (Join-Path $hostDir 'ready.json')
    Write-Step "client host ready on port $script:ServerPort ($hostDir)"
    $until = (Get-Date).AddHours(2)
    while (-not (Test-Path (Join-Path $hostDir 'stop'))) {
        if (-not (Test-ServerRunning)) { throw 'the server exited while hosting the client' }
        if ($owner -and $owner.HasExited) { Write-Step "the client run (pid $($owner.Id)) is gone; stopping the host"; break }
        if ((Get-Date) -gt $until) { throw 'client host timed out (2 h)' }
        foreach ($cmd in Get-ChildItem $hostDir -Filter 'cmd-*.txt' -ErrorAction SilentlyContinue | Sort-Object Name) {
            $text = (Get-Content $cmd.FullName -Raw).Trim()
            Remove-Item $cmd.FullName -Force
            if ($text) { Send-Console $text }
        }
        Wait-Seconds 1
    }
    Write-Step 'client done; stopping the host'
}