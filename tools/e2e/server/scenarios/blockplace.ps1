# U2 blockplace: placing a carried block with a right-click (SPEC §3.6) on a real BDS: placement
# target, state recompute compared with vanilla placement, kept states, failures (obstruction,
# non-replaceable, protection, height), water, chest contents, double chest re-pairing, skull
# rotation and the cancelled use-block path.
$r = Invoke-SelfTest 'blockplace.*'
if ($r.Fail -gt 0) { throw "$($r.Fail) blockplace selftest(s) failed" }
if ($r.Pass -lt 10) { throw "expected 10 blockplace selftests to pass, got $($r.Pass)" }
