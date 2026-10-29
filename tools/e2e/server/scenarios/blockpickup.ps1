# U1 block pickup (docs/SPEC.md §3.1) on a real BDS: every check (allowed and refused), chests with
# items, double chest halves, water, creative / survival and the Slowness amplifier. The pickups go
# through the real use-block path (simulated player interact).
$r = Invoke-SelfTest 'blockpickup.*'
if ($r.Fail -gt 0) { throw "$($r.Fail) blockpickup selftest(s) failed" }
if ($r.Pass -lt 12) { throw "expected at least 12 blockpickup selftests to pass, got $($r.Pass)" }
# One log line per pickup (the line e2e scenarios and admins can wait for).
Wait-LogLine 'carry: .* picked up block minecraft:chest at -?\d+ -?\d+ -?\d+' 5 | Out-Null
