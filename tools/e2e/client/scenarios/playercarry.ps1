# Carrying players (U5) with a real client:
#  1. the real player carries a SimulatedPlayer: screenshots in first and third person show it
#     0.6 in front of and 0.4 above the carrier's feet;
#  2. a SimulatedPlayer carries the real player; the real player sneaks and gets off, which
#     releases the carrier.
# Screenshots: out/playercarry/{carry-first,carry-third,carried-first,carried-third,dismounted}.png

$F5 = 0x74
function Toggle-Perspective([int]$times = 1) {
    for ($i = 0; $i -lt $times; $i++) {
        Focus-Game
        [BsE2E.Native]::Key([uint16]$F5, $false); Start-Sleep -Milliseconds 80; [BsE2E.Native]::Key([uint16]$F5, $true)
        Start-Sleep -Milliseconds 600
    }
}

# Starts a selftest without waiting, so the scenario can act while it runs.
function Start-SelfTest([string]$Glob) {
    $mark = Get-LogMark
    Send-SelfTestCommand (Format-SelfTestCommand $Glob)
    return $mark
}
function Complete-SelfTest([int]$Mark, [string]$Glob, [int]$TimeoutSeconds = 120) {
    try { Wait-LogLine $script:SelfTestDoneRegex $TimeoutSeconds -Since $Mark | Out-Null } catch { Write-Step "selftest '$Glob' did not finish: $_" }
    $r = ConvertFrom-SelfTestLines (Get-ServerLog -Since $Mark) $Glob
    Register-SelfTestResult $r | Out-Null
    return $r
}

# Look straight ahead (level), so the carried player is in view.
Invoke-Chat '/tp @s ~ ~ ~ 0 0'
Start-Sleep -Seconds 1

# 1. The real player carries a SimulatedPlayer.
$mark = Start-SelfTest 'playercarry.e2e_carry'
Wait-LogLine 'CARRYING READY' 60 -Since $mark | Out-Null
Start-Sleep -Seconds 2
Save-Screenshot 'carry-first' | Out-Null
Toggle-Perspective 1
Start-Sleep -Seconds 1
Save-Screenshot 'carry-third' | Out-Null
Toggle-Perspective 2
$r = Complete-SelfTest $mark 'playercarry.e2e_carry'
if ($r.Fail -gt 0 -or $r.Pass -lt 1) { throw 'playercarry.e2e_carry failed' }
Wait-LogLine 'placed player lo_carried' 10 -Since $mark | Out-Null

# 2. A SimulatedPlayer carries the real player; sneaking gets off.
Invoke-Chat '/tp @s ~ ~ ~ 0 0'
Start-Sleep -Seconds 1
$mark = Start-SelfTest 'playercarry.e2e_carried'
Wait-LogLine 'CARRIED READY' 60 -Since $mark | Out-Null
Start-Sleep -Seconds 2
Save-Screenshot 'carried-first' | Out-Null
Toggle-Perspective 1
Start-Sleep -Seconds 1
Save-Screenshot 'carried-third' | Out-Null
Toggle-Perspective 2
Focus-Game
Press-Key SHIFT 600
$hit = Wait-LogLine 'released player .* \((dismounted|test end)\)' 40 -Since $mark
Start-Sleep -Seconds 1
Save-Screenshot 'dismounted' | Out-Null
$r = Complete-SelfTest $mark 'playercarry.e2e_carried'
if ($r.Fail -gt 0 -or $r.Pass -lt 1) { throw 'playercarry.e2e_carried failed' }
if ($hit -notmatch '\(dismounted\)') { throw "sneaking did not get the real player off the carrier: $hit" }
