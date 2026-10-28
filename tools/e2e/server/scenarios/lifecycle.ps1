# U7 lifecycle: death with / without keepInventory, carried player leaving, logout / login,
# dimension change and the /carryon debug | clear | place commands, on a real BDS.
$mark = Get-LogMark
$r = Invoke-SelfTest 'lifecycle.*'
if ($r.Fail -gt 0) { throw "$($r.Fail) lifecycle selftest(s) failed" }
if ($r.Pass -lt 9) { throw "expected at least 9 lifecycle selftests to pass, got $($r.Pass)" }

# The command messages and actions are logged (the selftests ran them through real command origins).
Wait-LogLine 'Placed 1 Item!' 10 -Since $mark | Out-Null
Wait-LogLine 'Cleared \d+ Items?!' 10 -Since $mark | Out-Null
Wait-LogLine 'carry: placed carried block minecraft:barrel of \S+ at -?\d+ -?\d+ -?\d+ \(command\)' 10 -Since $mark | Out-Null
Wait-LogLine 'carry: placed block minecraft:barrel of \S+ at -?\d+ -?\d+ -?\d+ \(death\)' 10 -Since $mark | Out-Null
Wait-LogLine 'carry: debug \S+: Block: minecraft:barrel' 10 -Since $mark | Out-Null

# From the real console: targets are required (no player of its own) and an empty selector is an error.
$mark = Get-LogMark
Send-Console 'carryon clear @a'
Wait-LogLine 'No targets matched selector|Cleared \d+ Items?!' 30 -Since $mark | Out-Null
$mark = Get-LogMark
Send-Console 'carryon place'
Wait-LogLine 'needs a player' 30 -Since $mark | Out-Null
