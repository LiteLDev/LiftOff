# Every selftest of every unit in one world (integration check).
$r = Invoke-SelfTest -TimeoutSeconds 1800
if ($r.Fail -gt 0) { throw "$($r.Fail) selftest(s) failed" }
