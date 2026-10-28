# U13 armpose: pose values, per-viewer packets (vanilla / modded / self), stand-ins, view range, the
# resource-pack animation table. Logic only; the client scenario armpose shows the result.
$r = Invoke-SelfTest 'armpose.*'
if ($r.Pass -lt 9) { throw "expected at least 9 armpose selftests to pass, got $($r.Pass)" }
