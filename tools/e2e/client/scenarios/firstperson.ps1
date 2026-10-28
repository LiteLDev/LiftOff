# U10 first person: the local player is given a carry (chest with items, furnace, pig) by the
# firstperson.* selftests on the integrated server; each one is drawn in first person instead of the
# hand, screenshotted while the test holds it, and released afterwards. The chest is also looked at
# in both third-person views (no first-person drawing there, no crash).
# Screenshots: out/firstperson/{hud,chest,chest-third-back,chest-third-front,furnace,pig,after}.png
Wait-LogLine 'net: the server runs LiftOff' 120 | Out-Null
Start-Sleep -Seconds 5   # let the HUD settle before the first chat command
Save-Screenshot 'hud' | Out-Null

# Runs one selftest; `$During` runs once the test holds its carry (first frame drawn).
function Invoke-HeldSelfTest([string]$Name, [scriptblock]$During) {
    $mark = Get-LogMark
    Send-SelfTestCommand (Format-SelfTestCommand $Name)
    try {
        Wait-LogLine "SELFTEST INFO ${Name}: holding|SELFTEST (PASS|FAIL) $Name|$script:CommandErrorRegex" 30 -Since $mark | Out-Null
        & $During
        Wait-LogLine "$script:SelfTestDoneRegex|$script:CommandErrorRegex" 60 -Since $mark | Out-Null
    } catch {
        Write-Step "selftest '$Name' did not finish: $_"
    }
    $r = ConvertFrom-SelfTestLines (Get-LeviLog -Since $mark) $Name
    Register-SelfTestResult $r | Out-Null
    return $r
}

Invoke-SelfTest 'firstperson.is_chest' -TimeoutSeconds 60 | Out-Null
Invoke-SelfTest 'firstperson.transforms' -TimeoutSeconds 60 | Out-Null

Invoke-HeldSelfTest 'firstperson.chest' {
    Start-Sleep -Milliseconds 300
    Save-Screenshot 'chest' | Out-Null
    Press-Key F5; Start-Sleep -Milliseconds 500
    Save-Screenshot 'chest-third-back' | Out-Null
    Press-Key F5; Start-Sleep -Milliseconds 500
    Save-Screenshot 'chest-third-front' | Out-Null
    Press-Key F5; Start-Sleep -Milliseconds 300
} | Out-Null
Reset-CameraPerspective

Invoke-HeldSelfTest 'firstperson.furnace' {
    Start-Sleep -Milliseconds 300
    Save-Screenshot 'furnace' | Out-Null
} | Out-Null

Invoke-HeldSelfTest 'firstperson.pig' {
    Start-Sleep -Milliseconds 300
    Save-Screenshot 'pig' | Out-Null
} | Out-Null

Save-Screenshot 'after' | Out-Null
Wait-LogLine 'firstperson: drawing block minecraft:chest' 5 | Out-Null
Wait-LogLine 'firstperson: drawing entity minecraft:pig' 5 | Out-Null
