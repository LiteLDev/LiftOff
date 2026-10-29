# Seeds Carry On scripts (config/scripts/lo_e2e/...) before the server boots.
# 3 valid scripts (one in a sub folder) and 1 invalid one; the scenario adds a 4th and reloads.
$cfgDir = Join-Path $script:SlotDir "plugins\$script:ModName\config"
$scriptDir = Join-Path $cfgDir 'scripts\lo_e2e'
New-Item -ItemType Directory -Force (Join-Path $scriptDir 'sub') | Out-Null

@'
{
  "priority": 5,
  "object": { "block": { "name": "minecraft:barrel", "hardness": ">=1" } },
  "conditions": { "gamemode": "<=1" },
  "effects": { "commandPickup": "tag @s add lo_e2e_picked", "commandPlace": "tag @s remove lo_e2e_picked" }
}
'@ | Set-Content -Encoding UTF8 (Join-Path $scriptDir 'pickup.json')

@'
{
  "override_checks": true,
  "object": { "block": { "name": "minecraft:stone" } }
}
'@ | Set-Content -Encoding UTF8 (Join-Path $scriptDir 'low.json')

@'
{
  "priority": 1,
  "object": { "entity": { "name": "minecraft:pig", "nbt": { "CustomName": "lo" } } },
  "render": { "name_entity": "minecraft:cow", "scale": "x=0.5,y=0.5,z=0.5", "render_left_arm": false }
}
'@ | Set-Content -Encoding UTF8 (Join-Path $scriptDir 'sub\entity.json')

# Invalid: both block and entity.
@'
{
  "object": { "block": { "name": "minecraft:chest" }, "entity": { "name": "minecraft:pig" } },
  "render": {}
}
'@ | Set-Content -Encoding UTF8 (Join-Path $scriptDir 'invalid.json')
