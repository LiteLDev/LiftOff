# U13 armpose: the carry arm pose and (remote-vanilla) the stand-in of the carried thing.
# The selftest armpose.showcase (only active for a player tagged lo_armpose_showcase) puts three
# simulated carriers in front of the player (chest, pig, sneaking with a barrel) and gives the
# player a chest, so the screenshots show other carriers and the player itself (third person).
#   local / remote-mod: modded client, native exact pose, carried things drawn by U10/U11 (if present)
#   remote-vanilla:     vanilla client, pose from the server (AnimateEntityPacket) + stand-ins
# Screenshots: out/armpose*/<mode>-{carriers,first-person,self-front,self-sneak,self-back}.png
$m = $script:Mode
Invoke-Chat '/time set noon'
Invoke-Chat '/tag @s add lo_armpose_showcase'
$mark = Get-LogMark
Invoke-Chat '/carryon selftest "armpose.showcase"'
Wait-LogLine 'SELFTEST INFO armpose.showcase: showcase ready' 120 -Since $mark | Out-Null
Start-Sleep -Seconds 4
Save-Screenshot "$m-carriers" | Out-Null

Move-Mouse 0.5 0.5 -Relative
Invoke-Chat '/tp @s ~ ~ ~ 0 60'
Start-Sleep -Seconds 2
Save-Screenshot "$m-first-person" | Out-Null

Invoke-Chat '/tp @s ~ ~ ~ 0 0'
Invoke-Chat '/camera @s set minecraft:third_person_front'
Start-Sleep -Seconds 2
Save-Screenshot "$m-self-front" | Out-Null
Send-HeadlessKey -Key SHIFT -Action down
Start-Sleep -Seconds 1
Save-Screenshot "$m-self-sneak" | Out-Null
Send-HeadlessKey -Key SHIFT -Action up
Start-Sleep -Milliseconds 500
Invoke-Chat '/camera @s set minecraft:third_person'
Start-Sleep -Seconds 2
Save-Screenshot "$m-self-back" | Out-Null
Invoke-Chat '/camera @s clear'

Wait-LogLine 'SELFTEST DONE' 120 -Since $mark | Out-Null
$fail = Get-LeviLog -Since $mark | Where-Object { $_ -match 'SELFTEST FAIL armpose\.showcase' }
if ($fail) { throw "armpose.showcase failed: $fail" }
