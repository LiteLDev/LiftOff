# U13 renderArms = false on a modded client (prelaunch writes carryon_client.json). The server must not
# play the arm pose to this client: the carriers in front of it and the player itself (third person)
# keep their arms down. Local mode then turns the setting back on with /carryon reload (the client
# reloads its config there): the client says hello again and the pose appears.
# Screenshots: out/armpose-noarms*/<mode>-{carriers,self-front}-off.png, local also -on.png
$m = $script:Mode
Invoke-Chat '/time set noon'
Invoke-Chat '/tag @s add lo_armpose_showcase'
$mark = Get-LogMark
Invoke-Chat '/carryon selftest "armpose.showcase"'
Wait-LogLine 'SELFTEST INFO armpose.showcase: showcase ready' 120 -Since $mark | Out-Null
Start-Sleep -Seconds 4
Save-Screenshot "$m-carriers-off" | Out-Null
Invoke-Chat '/camera @s set minecraft:third_person_front'
Start-Sleep -Seconds 2
Save-Screenshot "$m-self-front-off" | Out-Null
Invoke-Chat '/camera @s clear'

if ($m -eq 'local') {
    $cfg = Join-Path (Join-Path (Join-Path $script:ModsDir $script:ModName) 'config') 'carryon_client.json'
    (Get-Content $cfg -Raw) -replace '"renderArms"\s*:\s*false', '"renderArms": true' | Set-Content -Encoding utf8NoBOM $cfg
    Invoke-Chat '/carryon reload'
    Start-Sleep -Seconds 3
    Save-Screenshot "$m-carriers-on" | Out-Null
    Invoke-Chat '/camera @s set minecraft:third_person_front'
    Start-Sleep -Seconds 2
    Save-Screenshot "$m-self-front-on" | Out-Null
    Invoke-Chat '/camera @s clear'
}

Wait-LogLine 'SELFTEST DONE' 180 -Since $mark | Out-Null
$fail = Get-LeviLog -Since $mark | Where-Object { $_ -match 'SELFTEST FAIL armpose\.showcase' }
if ($fail) { throw "armpose.showcase failed: $fail" }
