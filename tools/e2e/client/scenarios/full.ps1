# Every selftest of every unit on the integrated server of a local world (integration check),
# with screenshots before and after. Screenshots: out/full/{hud,selftest}.png
Save-Screenshot 'hud' | Out-Null
# The client mod said hello to the integrated server and was welcomed (carry protocol, U0).
Wait-LogLine 'net: the server runs LiftOff' 60 | Out-Null
Wait-LogLine 'net: .* joined with the LiftOff client mod' 10 | Out-Null
$r = Invoke-SelfTest -TimeoutSeconds 1800
Save-Screenshot 'selftest' | Out-Null
if ($r.Fail -gt 0) { throw "$($r.Fail) selftest(s) failed" }
