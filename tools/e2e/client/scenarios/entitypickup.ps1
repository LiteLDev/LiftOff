# Entity pickup (U3) with a real client: sneak + right-click a pig with empty hands picks it up.
# Meant for `-Mode remote-vanilla` (a vanilla client on BDS: does it send the interact transaction for
# a mob without an interaction?). In `local` / `remote-mod` the Carry key comes from the client mod
# (U9), not from sneaking.
# Screenshots: out/entitypickup/{before,after}.png
function Send-Cmd([string]$cmd) {
    if ($script:Mode -ne 'local') { Send-HostConsole $cmd; Start-Sleep -Milliseconds 600 }
    else { Invoke-Chat "/$cmd" }
}

Send-Cmd 'gamemode survival @a'
Send-Cmd 'clear @a'
Send-Cmd 'kill @e[type=pig]'
# Player at (0.5, -60, 0.5) looking at a frozen pig 1.5 blocks south (reach 2.5).
Send-Cmd 'summon pig 0.5 -60 2.0'
Send-Cmd 'effect @e[type=pig] slowness 600 255 true'
Send-Cmd 'tp @e[type=pig] 0.5 -60 2.0'           # it may have walked off before the Slowness
Send-Cmd 'tp @a 0.5 -60 0.5 0 45'   # yaw 0 = south, pitch 45 down: the crosshair is on the pig
Start-Sleep -Seconds 2
Save-Screenshot 'before' | Out-Null

$mark = Get-LogMark
Send-BeDDKey -Key SHIFT -Action down
Start-Sleep -Milliseconds 600
Send-BeDDRaw -Action click -Button right -HoldMs 120
Start-Sleep -Milliseconds 800
Send-BeDDKey -Key SHIFT -Action up
$line = Wait-LogLine 'carry: \S+ picked up entity minecraft:pig' 15 -Since $mark
Write-Host "[e2e] $line"
Start-Sleep -Seconds 3
Save-Screenshot 'after' | Out-Null
# The client must stay connected (sound, swing, Slowness and the removed pig are sent to it).
if (Get-LeviLog -Since $mark | Where-Object { $_ -match 'Player disconnected' }) { throw 'the client was disconnected after the pickup' }
