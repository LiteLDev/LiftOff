# Entity pickup (U3, SPEC §3.2): checks, capture + removal, Slowness, leash, passengers and the real
# GameMode::interact path (SimulatedPlayer::simulateInteract), on a real BDS.
$r = Invoke-SelfTest 'entitypickup.*'
if ($r.Fail -gt 0) { throw "$($r.Fail) entitypickup selftest(s) failed" }
if ($r.Pass -lt 14) { throw "expected 14 entitypickup selftests to pass, got $($r.Pass)" }
Wait-LogLine 'carry: \S+ picked up entity minecraft:cow' 5 | Out-Null
