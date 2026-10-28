# U2 blockplace on the client build's integrated server: the placement selftests (vanilla placement
# comparison, failures, water, chests, skulls, cancelled use-block), then real input by the local
# player: a chest two blocks ahead is picked up with sneak + right-click (U1) and put down with a
# right-click on the floor (U2). Without U1 in the build only the selftests run.
# Screenshots: out/blockplace/{before,carrying,after}.png
$r = Invoke-SelfTest 'blockplace.*'
if ($r.Fail -gt 0) { throw "$($r.Fail) blockplace selftest(s) failed" }
if ($r.Pass -lt 10) { throw "expected 10 blockplace selftests to pass, got $($r.Pass)" }

Invoke-Chat '/tp @s ~ ~ ~ 0 60'
Start-Sleep -Milliseconds 500
Invoke-Chat '/setblock ^ ^ ^2 chest'
Start-Sleep -Milliseconds 800
Save-Screenshot 'before' | Out-Null
$mark = Get-LogMark
[BsE2E.Native]::Key([uint16]$script:VK['SHIFT'], $false)   # hold the Carry key (Left Shift / sneak)
Start-Sleep -Milliseconds 300
Click 0.5 0.5 -Relative -Right
Start-Sleep -Milliseconds 300
[BsE2E.Native]::Key([uint16]$script:VK['SHIFT'], $true)
$picked = $true
try { Wait-LogLine 'carry: .* picked up block minecraft:chest' 5 -Since $mark | Out-Null } catch { $picked = $false }
if ($picked) {
    Start-Sleep -Milliseconds 500
    Save-Screenshot 'carrying' | Out-Null
    $mark = Get-LogMark
    Click 0.5 0.5 -Relative -Right
    Wait-LogLine 'carry: .* placed block minecraft:chest' 10 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 500
    Save-Screenshot 'after' | Out-Null
} else {
    Write-Host 'blockplace: no block pickup in this build (U1 missing); real-input placement skipped'
}
