# U4 entityplace: placing a carried entity (SPEC §3.7) and entity stacking (§3.8) on a real BDS,
# through the real use-block / entity-interact paths of SimulatedPlayers.
$r = Invoke-SelfTest 'entityplace.*'
if ($r.Fail -gt 0) { throw "$($r.Fail) entityplace selftest(s) failed" }
if ($r.Pass -lt 10) { throw "expected at least 10 entityplace selftests to pass, got $($r.Pass)" }
