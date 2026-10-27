<#
.SYNOPSIS
  (Re)build the LO-Test client world fixture (fixtures\LO-Test) from any Bedrock world.

.DESCRIPTION
  Only level.dat is taken from the source world (it carries the version/format fields the game
  expects); the terrain (db\) is dropped, so the game generates a fresh flat world on first
  load. The level.dat is patched to: name LO-Test, flat (classic layers), fixed seed,
  survival, peaceful, cheats on, always day, no weather, no mob spawning, coordinates shown.

  The committed fixture was built from BEI's fixture world:
    pwsh tools/e2e/client/fixtures/build-fixture.ps1 -Source <bei>\tools\e2e\fixtures\BEI-Test.zip
  Rebuild it when a game update makes the old level.dat too old for the game, from a world
  created in the current game version (a folder under ...\minecraftWorlds or a .zip/.mcworld).
#>
param([Parameter(Mandatory)] [string]$Source)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../../common/nbt.ps1"

$tmp = $null
if (Test-Path $Source -PathType Leaf) {
    # .mcworld is a zip; Expand-Archive only accepts the .zip extension.
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "lo-fixture-$PID"
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $zip = Join-Path $tmp 'world.zip'
    Copy-Item -LiteralPath $Source $zip
    Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $tmp 'w') -Force
    $Source = Join-Path $tmp 'w'
}
try {
    # level.dat may sit at the root or one folder down.
    $levelDat = Get-ChildItem $Source -Recurse -Filter 'level.dat' -File | Sort-Object { $_.FullName.Length } | Select-Object -First 1
    if (-not $levelDat) { throw "no level.dat found in $Source" }
    $ld = Read-LevelDat $levelDat.FullName
    $r = $ld.Root
    $flat = '{"biome_id":1,"block_layers":[{"block_name":"minecraft:bedrock","count":1},{"block_name":"minecraft:dirt","count":2},{"block_name":"minecraft:grass_block","count":1}],"encoding_version":6,"preset_id":"ClassicFlat","world_version":"version.post_1_18"}'
    Set-NbtValue $r 'LevelName' 8 'LO-Test'
    Set-NbtValue $r 'Generator' 3 2            # 2 = flat
    Set-NbtValue $r 'FlatWorldLayers' 8 $flat
    Set-NbtValue $r 'RandomSeed' 4 ([long]20260521)
    Set-NbtValue $r 'GameType' 3 0             # survival
    Set-NbtValue $r 'Difficulty' 3 0           # peaceful
    Set-NbtValue $r 'cheatsEnabled' 1 1
    Set-NbtValue $r 'commandsEnabled' 1 1
    Set-NbtValue $r 'dodaylightcycle' 1 0
    Set-NbtValue $r 'Time' 4 ([long]6000)      # noon
    Set-NbtValue $r 'doweathercycle' 1 0
    Set-NbtValue $r 'rainLevel' 5 ([single]0)
    Set-NbtValue $r 'lightningLevel' 5 ([single]0)
    Set-NbtValue $r 'domobspawning' 1 0
    Set-NbtValue $r 'spawnMobs' 1 0
    Set-NbtValue $r 'showcoordinates' 1 1
    Set-NbtValue $r 'keepinventory' 1 1
    Set-NbtValue $r 'SpawnX' 3 0
    Set-NbtValue $r 'SpawnY' 3 32767           # "find a safe spot"
    Set-NbtValue $r 'SpawnZ' 3 0
    Set-NbtValue $r 'currentTick' 4 ([long]0)
    Set-NbtValue $r 'LastPlayed' 4 ([long]0)   # set to "now" by Restore-TestWorld
    Set-NbtValue $r 'texturePacksRequired' 1 0

    $out = Join-Path $PSScriptRoot 'LO-Test'
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    New-Item -ItemType Directory $out | Out-Null
    Write-LevelDat $ld (Join-Path $out 'level.dat')
    Set-Content -Path (Join-Path $out 'levelname.txt') -Value 'LO-Test' -NoNewline -Encoding ascii
    Set-Content -Path (Join-Path $out 'world_behavior_packs.json') -Value '[]' -NoNewline -Encoding ascii
    Set-Content -Path (Join-Path $out 'world_resource_packs.json') -Value '[]' -NoNewline -Encoding ascii
    Write-Host "fixture written to $out"
} finally {
    if ($tmp) { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
}
