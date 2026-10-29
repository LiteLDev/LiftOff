# U6 on a real client (local world, integrated server): while carrying, number keys and the scroll
# wheel do not change the hotbar slot (the server refuses / snaps it back), the inventory does not
# open, and the slot is still the locked one at the end.
# Screenshots: out/restrictions/{carrying,after-keys,after-inventory,done}.png
Invoke-Chat '/tag @s add lo_e2e_restrictions'
$mark = Get-LogMark
Send-SelfTestCommand 'carryon selftest "restrictions.real_player"'
Wait-LogLine 'restrictions: e2e carry ready on' 60 -Since $mark | Out-Null
Start-Sleep -Milliseconds 500
Save-Screenshot 'carrying' | Out-Null

Focus-Game
Press-Key '3'
Press-Key '7'
Scroll 2
# The server snaps the slot back; with the U9 client lock the client already refuses the change.
Wait-LogLine '(restrictions: kept .* on hotbar slot|clientinput: hotbar slot change to \d blocked while carrying)' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
Save-Screenshot 'after-keys' | Out-Null

Open-Inventory
Save-Screenshot 'after-inventory' | Out-Null
Wait-LogLine 'restrictions: (refused the inventory|closed the container screen)' 20 -Since $mark | Out-Null

Invoke-Chat '/tag @s remove lo_e2e_restrictions'
$line = Wait-LogLine 'SELFTEST (PASS|FAIL) restrictions\.real_player' 60 -Since $mark
Save-Screenshot 'done' | Out-Null
if ($line -match 'SELFTEST FAIL') { throw "restrictions.real_player failed: $line" }
Wait-LogLine 'SELFTEST DONE pass=1 fail=0' 30 -Since $mark | Out-Null
