# U11 third-person rendering of carried objects (docs/SPEC.md §4.3).
#
# The thirdperson.scene_* selftests run on the server (the integrated server, or the BDS host with
# -Mode remote-mod, where the carries reach this client only through CarrySync): they put the player in
# front of simulated carriers and hold each scene for a few seconds, logging
# "SELFTEST INFO thirdperson.<test>: ready <scene>"; this scenario screenshots every scene.
# Locally the client build's selftests also check what the renderer drew and switch the camera for the
# player's own carry; remotely this scenario presses F5 instead.
#
# Screenshots: out/thirdperson/{blocks,blocks-sneaking,entities,entities-sneaking,local-back,local-front}.png
$remote = $script:Mode -ne 'local'

Wait-LogLine 'net: the server runs LiftOff' 90 | Out-Null

# Runs one scene test: waits for each "ready <scene>" line and screenshots it, then records the result.
function Invoke-Scene([string]$Test, [string[]]$Scenes, [scriptblock]$OnReady = $null) {
    $mark = Get-LogMark
    Send-SelfTestCommand (Format-SelfTestCommand "thirdperson.$Test")
    foreach ($scene in $Scenes) {
        Wait-LogLine "thirdperson\.$([regex]::Escape($Test)): ready $([regex]::Escape($scene))\b" 120 -Since $mark | Out-Null
        if ($OnReady) { & $OnReady $scene } else { Start-Sleep -Milliseconds 700; Save-Screenshot $scene | Out-Null }
    }
    try {
        Wait-LogLine "$script:SelfTestDoneRegex|$script:CommandErrorRegex" 120 -Since $mark | Out-Null
    } catch {
        Write-Step "selftest thirdperson.$Test did not finish: $_"
    }
    $r = ConvertFrom-SelfTestLines (Get-ServerLog -Since $mark) "thirdperson.$Test"
    Register-SelfTestResult $r | Out-Null
    foreach ($l in Get-ServerLog -Since $mark) { if ($l -match 'SELFTEST INFO thirdperson\.\S+: \S+: centre') { Write-Step $l.Trim() } }
}

Invoke-Scene 'scene_blocks' @('blocks')
Invoke-Scene 'scene_blocks_sneaking' @('blocks-sneaking')
Invoke-Scene 'scene_entities' @('entities', 'entities-sneaking')

if ($remote) {
    # Third-person back (F5 once) and front (F5 twice), then back to first person.
    Invoke-Scene 'scene_local' @('local') {
        param($scene)
        Focus-Game
        Press-Key F5; Start-Sleep -Milliseconds 900
        Save-Screenshot 'local-back' | Out-Null
        Press-Key F5; Start-Sleep -Milliseconds 900
        Save-Screenshot 'local-front' | Out-Null
        Press-Key F5; Start-Sleep -Milliseconds 400
    }
} else {
    Invoke-Scene 'scene_local' @('local-back', 'local-front')
}

# The client drew what the server synced (both modes): the renderer logs one line per carrier and carry.
$drawn = @(Get-LeviLog | Where-Object { $_ -match 'thirdperson: drawing .*carried (block|entity)' })
Write-Step "renderer lines: $($drawn.Count)"
if (-not ($drawn -match 'carried block')) { throw 'the client never drew a carried block' }
if (-not ($drawn -match 'carried entity')) { throw 'the client never drew a carried entity' }
