# Writes assets/pack/rp/animations/liftoff_carry.animation.json: the player animations of the carry arm
# pose (docs/SPEC.md §4.5, U13). The names and constants must match src/core/ArmPose.h.
#
#   pwsh tools/armpose/animations.ps1
#
# Families (Java radians -> Bedrock degrees, same signs on every axis):
#   animation.liftoff.carry.<right|left>.<x|y|z>.<m|p><i>  one axis set absolutely to i * 0.05 rad
#   animation.liftoff.carry.<right|left>.xc.<m|p><i>       x plus Carry On's crouch term (-0.2 rad)
#   animation.liftoff.carry.first_person.right             first-person right arm (left arm is hidden)
#   animation.liftoff.carry.none                           empty (ends a slot)
# Every third-person value is skipped while swimming / gliding (Carry On keeps the vanilla pose) and in
# first person. While sneaking Bedrock tilts the player's root 28 degrees forward (Java does not move the
# arms), so x subtracts it.

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$out  = Join-Path $root 'assets\pack\rp\animations\liftoff_carry.animation.json'

$step       = 0.05
$maxIndex   = 63
$sneakRoot  = 28.0
$crouchDeg  = 0.2 * 180.0 / [Math]::PI
$inv        = [Globalization.CultureInfo]::InvariantCulture

function Num([double]$v) { [Math]::Round($v, 4).ToString('0.####', $inv) }
function Suffix([int]$i) { if ($i -lt 0) { "m$(-$i)" } elseif ($i -gt 0) { "p$i" } else { '0' } }

$skip = '(v.is_first_person || q.is_swimming || q.is_gliding)'

# First-person right arm: position and rotation (absolute), modelled on the vanilla two-handed map
# pose so the hand reaches in front of the body as if holding the carried thing from below.
$fpPos = @(12.5, -9.0, 6.0)
$fpRot = @(77.5, 7.5, 160.0)

$anims = [System.Collections.Generic.List[string]]::new()
function Add-Anim([string]$name, [string]$bones) {
    $anims.Add("    `"$name`": {`"loop`": true, `"bones`": {$bones}}")
}

foreach ($side in 'right', 'left') {
    $bone = "${side}arm"
    for ($i = -$maxIndex; $i -le $maxIndex; $i++) {
        $deg = Num ($i * $step * 180.0 / [Math]::PI)
        $sx  = "-this + $deg - q.is_sneaking * $(Num $sneakRoot)"
        $sxc = "-this + $deg - q.is_sneaking * $(Num ($sneakRoot + $crouchDeg))"
        $s   = "-this + $deg"
        Add-Anim "animation.liftoff.carry.$side.x.$(Suffix $i)"  "`"$bone`": {`"rotation`": [`"$skip ? 0.0 : $sx`", 0.0, 0.0]}"
        Add-Anim "animation.liftoff.carry.$side.xc.$(Suffix $i)" "`"$bone`": {`"rotation`": [`"$skip ? 0.0 : $sxc`", 0.0, 0.0]}"
        Add-Anim "animation.liftoff.carry.$side.y.$(Suffix $i)"  "`"$bone`": {`"rotation`": [0.0, `"$skip ? 0.0 : $s`", 0.0]}"
        Add-Anim "animation.liftoff.carry.$side.z.$(Suffix $i)"  "`"$bone`": {`"rotation`": [0.0, 0.0, `"$skip ? 0.0 : $s`"]}"
    }
}

$fp = { param($v) "`"v.is_first_person ? -this + $(Num $v) : 0.0`"" }
Add-Anim 'animation.liftoff.carry.first_person.right' ("`"rightarm`": {`"position`": [$(& $fp $fpPos[0]), $(& $fp $fpPos[1]), $(& $fp $fpPos[2])], " +
    "`"rotation`": [$(& $fp $fpRot[0]), $(& $fp $fpRot[1]), $(& $fp $fpRot[2])]}")

# An animation without bones is rejected by the client, and a rejected entry drops the whole file:
# the empty animation adds 0 to one bone.
$anims.Add('    "animation.liftoff.carry.none": {"loop": true, "bones": {"rightarm": {"rotation": [0.0, 0.0, 0.0]}}}')

$text = "{`n  `"format_version`": `"1.8.0`",`n  `"animations`": {`n" + ($anims -join ",`n") + "`n  }`n}`n"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
[IO.File]::WriteAllText($out, $text, [Text.UTF8Encoding]::new($false))
$null = $text | ConvertFrom-Json  # must be valid JSON
Write-Host "wrote $($anims.Count) animations to $out"
