# Carry On scripts and custom pickup conditions (U8) on a real BDS: the seeded scripts load at start,
# the selftests cover matching, conditions, priority, override_checks, the commands and the custom
# pickup conditions, and /carryon reload picks up a new script file.
Wait-LogLine 'scripts: loaded 3 script\(s\), 1 invalid' 30 | Out-Null
$warn = Get-ServerLog | Select-String 'scripts: lo_e2e/invalid skipped' | Select-Object -First 1
if (-not $warn) { throw 'the invalid script was not reported' }

# Turn scripts on in the generated carryon.json (every key must be present) and reload.
$cfg = Join-Path $script:SlotDir "plugins\$script:ModName\config\carryon.json"
$json = Get-Content $cfg -Raw | ConvertFrom-Json
if ($json.settings.useScripts) { throw 'carryon.json: useScripts should default to false' }
$json.settings.useScripts = $true
$json | ConvertTo-Json -Depth 10 | Set-Content -Encoding UTF8 $cfg
$mark = Get-LogMark
Send-Console 'carryon reload'
Wait-LogLine 'scripts: loaded 3 script\(s\), 1 invalid' 30 -Since $mark | Out-Null

$r = Invoke-SelfTest 'scripts.*'
if ($r.Fail -gt 0) { throw "$($r.Fail) scripts selftest(s) failed" }
if ($r.Pass -lt 10) { throw "expected at least 10 scripts selftests to pass, got $($r.Pass)" }

# A new script file is picked up by /carryon reload.
@'
{ "object": { "entity": { "name": "minecraft:cow" } }, "effects": { "commandLoop": "say loop" } }
'@ | Set-Content -Encoding UTF8 (Join-Path $script:SlotDir "plugins\$script:ModName\config\scripts\lo_e2e\added.json")
$mark = Get-LogMark
Send-Console 'carryon reload'
Wait-LogLine 'scripts: loaded 4 script\(s\), 1 invalid' 30 -Since $mark | Out-Null

$r = Invoke-SelfTest 'scripts.files'
if ($r.Fail -gt 0 -or $r.Pass -lt 1) { throw 'scripts.files failed after the reload' }
