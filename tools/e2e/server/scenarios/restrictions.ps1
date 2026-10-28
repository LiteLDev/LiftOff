# U6 restrictions while carrying (hotbar lock, item pickup, pick block, use item, attack / destroy vs
# hitWhileCarrying, containers, dropCarriedWhenHit) on a real BDS with SimulatedPlayers.
$r = Invoke-SelfTest 'restrictions.*'
if ($r.Fail -gt 0) { throw "$($r.Fail) restrictions selftest(s) failed" }
if ($r.Pass -lt 9) { throw "expected 9 restrictions selftests to pass, got $($r.Pass)" }
