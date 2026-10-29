# U12 on a real BDS: override parsing / matching and script render resolve; the visual provider and the
# /carryon debug line never apply model overrides on a dedicated server (no client config, as in Java).
$r = Invoke-SelfTest 'modeloverrides.*'
if ($r.Fail -gt 0) { throw "$($r.Fail) modeloverrides selftest(s) failed" }
if ($r.Pass -lt 6) { throw "expected at least 6 modeloverrides selftests to pass, got $($r.Pass)" }
