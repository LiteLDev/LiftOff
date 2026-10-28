# U12: model overrides and script render overrides on the integrated server of a local world, including
# the client-thread path the renderers use (visualFor applies carryon_client.json's model overrides there).
$r = Invoke-SelfTest 'modeloverrides.*'
if ($r.Fail -gt 0) { throw "$($r.Fail) modeloverrides selftest(s) failed" }
if ($r.Pass -lt 6) { throw "expected at least 6 modeloverrides selftests to pass, got $($r.Pass)" }
