# Smoke: the mod is enabled (run.ps1 waited for "LiftOff enabled"), the player is in the
# LO-Test world and the foundation selftests pass on the integrated server.
# Screenshots: out/smoke/{hud,selftest}.png
Save-Screenshot 'hud' | Out-Null
$r = Invoke-SelfTest 'foundation.*'
if ($r.Pass -lt 4) { throw "expected at least 4 foundation selftests to pass, got $($r.Pass)" }
Save-Screenshot 'selftest' | Out-Null
