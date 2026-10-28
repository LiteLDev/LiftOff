# Every selftest of every unit on the integrated server of a local world (integration check),
# with screenshots before and after. Screenshots: out/full/{hud,selftest}.png
Save-Screenshot 'hud' | Out-Null
$r = Invoke-SelfTest -TimeoutSeconds 1800
Save-Screenshot 'selftest' | Out-Null
if ($r.Fail -gt 0) { throw "$($r.Fail) selftest(s) failed" }
