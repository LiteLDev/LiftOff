# U9 clientinput: the Carry key and the client restrictions while carrying, on a real client.
# Works in every mode: local (integrated server) and -Mode remote-mod (BDS); the server-side lines
# ("clientinput: <player> carry key down") come from the integrated server or the host.
#
# 1. Holding Shift (the default Carry key) sends the key state; the server logs it; releasing it too.
# 2. The player gets a carry (selftest fixture `clientinput.e2e_fixture`, tag lo_e2e_carry) on slot 3:
#    number keys and the wheel do not change the slot, E (inventory) is closed right away, middle click
#    (pick block) is cancelled; the server still sees the locked slot (tag lo_e2e_check).
# 3. Without the carry, the slot changes again.
# 4. Settings > Controls lists the "Carry" key.
# Screenshots: out/clientinput/{hud,key-held,carrying,keys-blocked,inventory-closed,uncarried,controls*}.png
Wait-LogLine 'net: the server runs LiftOff' 90 | Out-Null
Save-Screenshot 'hud' | Out-Null

function Invoke-Fixture([string]$Tag) {
    Invoke-Chat "/tag @s add $Tag"
    $r = Invoke-SelfTest 'clientinput.e2e_fixture'
    if ($r.Fail -gt 0 -or $r.Pass -lt 1) { throw "fixture ($Tag) failed: $($r.Failed | ForEach-Object { $_.Reason })" }
}

# ---- 1. the Carry key -----------------------------------------------------------------------------
Focus-Game
$mark = Get-LogMark
[BsE2E.Native]::Key([uint16]16, $false)   # Shift down (held)
try {
    Wait-LogLine 'clientinput: carry key down \(sent\)' 10 -Since $mark | Out-Null
    Wait-LogLine 'clientinput: .+ carry key down\s*$' 10 -Since $mark | Out-Null
    Save-Screenshot 'key-held' | Out-Null
    $mark = Get-LogMark
} finally {
    [BsE2E.Native]::Key([uint16]16, $true)    # Shift up
}
Wait-LogLine 'clientinput: carry key up \(sent\)' 10 -Since $mark | Out-Null
Wait-LogLine 'clientinput: .+ carry key up\s*$' 10 -Since $mark | Out-Null
Write-Step 'carry key down/up reached the server'

# ---- 2. carrying: hotbar lock, containers, pick block -----------------------------------------------
Press-Key '3'; Start-Sleep -Milliseconds 400       # slot index 2
$mark = Get-LogMark
Invoke-Fixture 'lo_e2e_carry'
try {
    Wait-LogLine 'clientinput: carrying; hotbar locked to slot 2' 10 -Since $mark | Out-Null
    Save-Screenshot 'carrying' | Out-Null

    $mark = Get-LogMark
    Press-Key '6'; Start-Sleep -Milliseconds 300
    Press-Key '9'; Start-Sleep -Milliseconds 300
    Scroll 2; Scroll -3
    Wait-LogLine 'clientinput: hotbar slot change to \d blocked while carrying' 10 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 500
    Save-Screenshot 'keys-blocked' | Out-Null

    $mark = Get-LogMark
    Focus-Game; Press-Key E; Start-Sleep -Milliseconds 1500
    Wait-LogLine 'clientinput: closed .+ while carrying' 10 -Since $mark | Out-Null
    Save-Screenshot 'inventory-closed' | Out-Null

    # Look at the ground and middle-click it (pick block).
    [BsE2E.Native]::MoveRelative(0, 400); Start-Sleep -Milliseconds 500
    $mark = Get-LogMark
    [BsE2E.Native]::MouseButton(2, $false); Start-Sleep -Milliseconds 80; [BsE2E.Native]::MouseButton(2, $true)
    Wait-LogLine 'clientinput: pick block cancelled while carrying' 10 -Since $mark | Out-Null
    Save-Screenshot 'pick-block' | Out-Null
    [BsE2E.Native]::MoveRelative(0, -400); Start-Sleep -Milliseconds 300

    Invoke-Fixture 'lo_e2e_check'                  # the server still sees slot 2
} finally {
    $mark = Get-LogMark
    Invoke-Fixture 'lo_e2e_uncarry'
}
Wait-LogLine 'clientinput: carry ended; hotbar unlocked' 10 -Since $mark | Out-Null

# ---- 3. without a carry everything is free again ------------------------------------------------------
$mark = Get-LogMark
Press-Key '6'; Start-Sleep -Milliseconds 500
if (Get-LeviLog -Since $mark | Where-Object { $_ -match 'blocked while carrying' }) { throw 'slot change blocked without a carry' }
Save-Screenshot 'uncarried' | Out-Null
Press-Key '1'; Start-Sleep -Milliseconds 300

$r = Invoke-SelfTest 'clientinput.*'
if ($r.Fail -gt 0) { throw "$($r.Fail) clientinput selftest(s) failed" }
