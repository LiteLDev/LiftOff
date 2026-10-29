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

# Mean absolute colour difference (0-255) of two screenshots in the lower middle of the screen, where
# the carried object is drawn (the vanilla hand is on the right, the hotbar below).
Add-Type -AssemblyName System.Drawing
function Get-RegionDiff([string]$A, [string]$B) {
    $ia = [System.Drawing.Bitmap]::new($A); $ib = [System.Drawing.Bitmap]::new($B)
    try {
        $sum = 0.0; $n = 0
        for ($x = [int]($ia.Width * 0.35); $x -lt [int]($ia.Width * 0.65); $x += 4) {
            for ($y = [int]($ia.Height * 0.55); $y -lt [int]($ia.Height * 0.85); $y += 4) {
                $p = $ia.GetPixel($x, $y); $q = $ib.GetPixel($x, $y)
                $sum += ([Math]::Abs($p.R - $q.R) + [Math]::Abs($p.G - $q.G) + [Math]::Abs($p.B - $q.B)) / 3.0
                $n++
            }
        }
        return $sum / $n
    } finally { $ia.Dispose(); $ib.Dispose() }
}
function Assert-Drawn([string]$What, [string]$Before, [string]$During) {
    if (-not $During) { throw "$What was never screenshotted" }
    $d = Get-RegionDiff $Before $During
    Write-Step ("{0}: lower-middle region differs by {1:N1} from the empty view" -f $What, $d)
    if ($d -lt 12) { throw "$What is not visible in first person (region diff $([Math]::Round($d, 1)))" }
}

Invoke-SelfTest 'firstperson.is_chest' -TimeoutSeconds 60 | Out-Null
Invoke-SelfTest 'firstperson.transforms' -TimeoutSeconds 60 | Out-Null

$empty = Save-Screenshot 'empty'
Invoke-HeldSelfTest 'firstperson.chest' {
    Start-Sleep -Milliseconds 300
    $script:chestShot = Save-Screenshot 'chest'
    Press-Key F5; Start-Sleep -Milliseconds 500
    Save-Screenshot 'chest-third-back' | Out-Null
    Press-Key F5; Start-Sleep -Milliseconds 500
    Save-Screenshot 'chest-third-front' | Out-Null
    Press-Key F5; Start-Sleep -Milliseconds 300
} | Out-Null
Reset-CameraPerspective   # options.txt only: the next launch starts in first person even if an F5 was lost

Invoke-HeldSelfTest 'firstperson.furnace' {
    Start-Sleep -Milliseconds 300
    Save-Screenshot 'furnace' | Out-Null
} | Out-Null

Invoke-HeldSelfTest 'firstperson.pig' {
    Start-Sleep -Milliseconds 300
    $script:pigShot = Save-Screenshot 'pig'
    # Looking down: Carry On's entity turns with the camera (it is drawn in view space).
    [BsE2E.Native]::MoveRelative(0, 300); Start-Sleep -Milliseconds 400
    Save-Screenshot 'pig-looking-down' | Out-Null
    [BsE2E.Native]::MoveRelative(0, -300); Start-Sleep -Milliseconds 300
    Press-Key F5; Start-Sleep -Milliseconds 500
    Save-Screenshot 'pig-third-back' | Out-Null
    Press-Key F5; Start-Sleep -Milliseconds 300
    Press-Key F5; Start-Sleep -Milliseconds 300
} | Out-Null

Invoke-HeldSelfTest 'firstperson.unseen_sheep' {
    Start-Sleep -Milliseconds 300
    $script:sheepShot = Save-Screenshot 'unseen-sheep'
} | Out-Null

Save-Screenshot 'after' | Out-Null

# Rejoining while carrying: the client never sees the pickup of the pig it carries after joining again.
Invoke-Chat '/tag @s add lo_fp_rejoin'
Invoke-SelfTest 'firstperson.rejoin_hold' -TimeoutSeconds 60 | Out-Null
Press-Key ESC; Start-Sleep -Milliseconds 1500
Save-Screenshot 'pause' | Out-Null
Click 0.293 0.667 -Relative                     # Save & Quit
Start-Sleep -Seconds 12                          # back at the title screen
Save-Screenshot 'left-world' | Out-Null
$rejoinMark = Get-LogMark
Enter-TestWorld
Wait-LogLine 'net: the server runs LiftOff' 120 -Since $rejoinMark | Out-Null
Start-Sleep -Seconds 3
Invoke-HeldSelfTest 'firstperson.rejoin_check' {
    Start-Sleep -Milliseconds 300
    $script:rejoinShot = Save-Screenshot 'pig-after-rejoin'
} | Out-Null
Assert-Drawn 'the pig carried across a rejoin' $empty $script:rejoinShot
Assert-Drawn 'the carried unseen sheep' $empty $script:sheepShot
Assert-Drawn 'the carried chest' $empty $script:chestShot
Assert-Drawn 'the carried pig' $empty $script:pigShot
foreach ($line in 'firstperson: drawing block minecraft:chest', 'firstperson: drawing entity minecraft:pig') {
    if (-not (Get-LeviLog | Where-Object { $_ -match $line })) { throw "missing log line '$line'" }
}
