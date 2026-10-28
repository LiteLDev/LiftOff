# LiftOff

[![Build](https://github.com/LiteLDev/LiftOff/actions/workflows/build.yml/badge.svg)](https://github.com/LiteLDev/LiftOff/actions/workflows/build.yml)
[![License](https://img.shields.io/github/license/LiteLDev/LiftOff)](COPYING.LESSER)

Pick up chests, blocks and mobs and carry them in your arms. LiftOff brings Tschipp's Java mod [Carry On](https://modrinth.com/mod/carry-on) to Bedrock, for dedicated servers and single-player worlds.

> [!NOTE]
> LiftOff is in early development. There is no release yet.

## How to play

- **Pick up**: with both hands empty, hold the Carry key (default Left Shift; on a vanilla client: sneak) and use
  (right-click) a block that has contents, such as a chest, or a mob. You carry it in both arms and walk slower,
  the heavier the load. Players can be carried too.
- **Put down**: use a block while carrying. Blocks keep their contents; mobs keep their name, health and data.
- **Stack**: use a mob while carrying a mob to put it on top.
- While carrying you cannot switch the hotbar slot, use items, open containers, break blocks or attack.

Servers can choose what may be carried (black/whitelists, scripts, custom pickup conditions); see
[docs/SPEC.md](docs/SPEC.md) for the complete behaviour.

## Install

You need Minecraft Bedrock Edition **1.26.51** for Windows (x64) with LeviLamina **26.51.x** (on a client, installed through [LeviLauncher](https://github.com/LiteLDev/LeviLauncher)).

With [lip](https://github.com/LiteLDev/lip):

```sh
lip install github.com/LiteLDev/LiftOff           # dedicated server
lip install github.com/LiteLDev/LiftOff#client    # client
```

Or download the zip from the [latest release](https://github.com/LiteLDev/LiftOff/releases/latest) and extract it next to `LeviLamina/`:

| Where | Zip | Folder |
| --- | --- | --- |
| Dedicated server | `liftoff-server-windows-x64.zip` | `plugins/LiftOff/` |
| Client (LeviLauncher) | `liftoff-client-windows-x64.zip` | `mods/LiftOff/` |

## Commands

| Command | Who | What it does |
| --- | --- | --- |
| `/carryon debug` | Everyone | Shows what you carry (block, state, NBT / entity, name, NBT). |
| `/carryon clear` | Everyone | Deletes what you carry without placing it. |
| `/carryon clear <targets>` | Operators | Deletes what the targets carry. |
| `/carryon place [targets]` | Operators | Puts down what you / the targets carry, at the nearest free spot. |
| `/carryon reload` | Operators, console | Reloads the configuration and translations. |
| `/carryon selftest ["glob"]` | Operators, console | Runs the in-game selftests. |

`/liftoff` is an alias of `/carryon`.

## Configuration

Settings live in `config/*.json` inside the mod folder (`plugins/LiftOff/` or `mods/LiftOff/`). Each file is created with defaults on first start; `/carryon reload` applies changes.

| File | Setting | Default | Meaning |
| --- | --- | --- | --- |
| `mod.json` | `language` | `en_US` | Language of server messages when a player's own language has no translation. |
| | `usePlayerLocale` | `true` | Use each player's game language when a translation exists. |
| | `requireResourcePack` | `true` | Dedicated server: players must accept the resource pack to join. |
| `carryon.json` | `settings.maxDistance` | `2.5` | How far away (blocks) something can be picked up. |
| | `settings.scaleDistance` | `true` | Scale the distance with the reach (longer in creative). |
| | `settings.maxEntityWidth` / `maxEntityHeight` | `1.5` / `2.5` | Largest mob that can be picked up in survival. |
| | `settings.relativePlayerScale` | `true` | Mob sizes relative to the carrier's size. |
| | `settings.heavyTiles` / `heavyEntities` | `true` | Heavier blocks (more contents) and bigger mobs slow you down more. |
| | `settings.blockSlownessMultiplier` / `entitySlownessMultiplier` | `1.0` | Scale the slowness. |
| | `settings.slownessInCreative` | `true` | Slow down creative players too. |
| | `settings.pickupAllBlocks` | `false` | Allow blocks without contents (stone, ...). |
| | `settings.pickupUnbreakableBlocks` | `false` | Allow unbreakable blocks in survival. |
| | `settings.pickupHostileMobs` | `false` | Allow monsters in survival. |
| | `settings.allowBabies` | `false` | Baby mobs ignore the entity lists. |
| | `settings.pickupPlayers` | `true` | Allow carrying players. |
| | `settings.stackableEntities` | `true` | Allow stacking mobs. |
| | `settings.maxEntityStackLimit` | `10` | Highest mob stack. |
| | `settings.entitySizeMattersStacking` | `true` | Only smaller (or equal) mobs go on top. |
| | `settings.hitWhileCarrying` | `false` | Allow breaking blocks and attacking while carrying. |
| | `settings.dropCarriedWhenHit` | `false` | Drop what you carry when you take damage. |
| | `settings.useScripts` | `false` | Enable scripts (`config/scripts/`). |
| | `settings.useWhitelistBlocks` / `useWhitelistEntities` / `useWhitelistStacking` | `false` | Use the whitelists instead of the blacklists. |
| | `settings.placementStateExceptions` | (list) | Block states recomputed when a block is put down. |
| | `blacklist.forbiddenTiles` / `forbiddenEntities` / `forbiddenStacking` | (lists) | What can never be picked up / stacked on. |
| | `whitelist.allowedBlocks` / `allowedEntities` / `allowedStacking` | `[]` | What can be, in whitelist mode. |
| | `customPickupConditions.customPickupConditionsBlocks` / `...Entities` | `[]` | Pickups that need a stage (player tag `carryon:stage:<name>`). |
| `carryon_client.json` | `settings.facePlayer` | `false` | Carried blocks face you. |
| | `settings.rotateEntitiesSideways` | `false` | Carried mobs are held sideways. |
| | `settings.renderArms` | `true` | Show the arms holding the load. |
| | `modelOverrides.modelOverrides` | (list) | Draw some blocks as another block or item. |

List entries are ids (`minecraft:chest`), wildcards (`minecraft:*_door`), tags (`#tag`) or ids with states (`minecraft:seagrass[sea_grass_type=double_top]`).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). [AGENTS.md](AGENTS.md) is the developer guide.

## Credits

LiftOff reproduces the behaviour of [Carry On](https://github.com/Tschipp/CarryOn) by Tschipp. It copies no code or assets from it.

## License

[LGPL-3.0-or-later](COPYING.LESSER)
