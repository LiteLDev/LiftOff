# LiftOff

[![Build](https://github.com/LiteLDev/LiftOff/actions/workflows/build.yml/badge.svg)](https://github.com/LiteLDev/LiftOff/actions/workflows/build.yml)
[![License](https://img.shields.io/github/license/LiteLDev/LiftOff)](COPYING.LESSER)

Pick up chests, blocks and mobs and carry them in your arms. LiftOff brings Tschipp's Java mod [Carry On](https://modrinth.com/mod/carry-on) to Bedrock, for dedicated servers and single-player worlds.

> [!NOTE]
> LiftOff is in early development. There is no release yet.

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
| `/carryon reload` | Operators, console | Reloads the configuration and translations. |
| `/carryon selftest ["glob"]` | Operators, console | Runs the in-game selftests. |

`/liftoff` is an alias of `/carryon`.

## Configuration

Settings live in `config/*.json` inside the mod folder (`plugins/LiftOff/` or `mods/LiftOff/`). Each file is created with defaults on first start.

| File | Setting | Default | Meaning |
| --- | --- | --- | --- |
| `mod.json` | `language` | `en_US` | Language of server messages when a player's own language has no translation. |
| | `usePlayerLocale` | `true` | Use each player's game language when a translation exists. |
| | `requireResourcePack` | `true` | Dedicated server: players must accept the resource pack to join. |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). [AGENTS.md](AGENTS.md) is the developer guide.

## Credits

LiftOff reproduces the behaviour of [Carry On](https://github.com/Tschipp/CarryOn) by Tschipp. It copies no code or assets from it.

## License

[LGPL-3.0-or-later](COPYING.LESSER)
