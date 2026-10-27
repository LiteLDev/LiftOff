# Smoke: the mod is enabled (run.ps1 already waited for "LiftOff enabled") and the
# foundation selftests pass (world storage, translations, configs, blocks, simulated players).
$r = Invoke-SelfTest 'foundation.*'
if ($r.Pass -lt 4) { throw "expected at least 4 foundation selftests to pass, got $($r.Pass)" }
