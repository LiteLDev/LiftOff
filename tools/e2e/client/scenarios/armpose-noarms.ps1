# U13 renderArms = false on a modded client (prelaunch writes carryon_client.json). The server must not
# play the arm pose to this client: the carriers in front of it and the player itself (third person)
# keep their arms down. Then the setting is turned back on by editing the file: in a local world with
# /carryon reload, on a dedicated server by the client noticing the changed file (live reload). The
# client says hello again, the server logs it and plays the pose.
# Screenshots: out/armpose-noarms*/<mode>-{carriers,self-front}-{off,on}.png
$m = $script:Mode
# The server (integrated or [host]) logged the setting from our hello.
Wait-LogLine 'net: \S+ has renderArms off' 60 | Out-Null
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

if ($m -ne 'remote-vanilla') {
    $cfg = Join-Path (Join-Path (Join-Path $script:ModsDir $script:ModName) 'config') 'carryon_client.json'
    $reload = Get-LogMark
    (Get-Content $cfg -Raw) -replace '"renderArms"\s*:\s*false', '"renderArms": true' | Set-Content -Encoding utf8NoBOM $cfg
    if ($m -eq 'local') {
        Invoke-Chat '/carryon reload'
    } else {
        Wait-LogLine 'client config: reloaded carryon_client.json' 30 -Since $reload | Out-Null
    }
    Wait-LogLine 'net: \S+ has renderArms on' 30 -Since $reload | Out-Null
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
