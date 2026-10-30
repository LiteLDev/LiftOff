# Contributing to LiftOff

Thanks for helping! Bug reports, ideas and pull requests are all welcome.

## Reporting bugs and requesting features

Use the [issue templates](https://github.com/LiteLDev/LiftOff/issues/new/choose). For bugs, include:

- the LiftOff version;
- the LeviLamina version;
- the Minecraft version;
- whether it happens on a dedicated server, in a local world, or with a vanilla client;
- steps to reproduce;
- the relevant part of the server console or `logs/latest.log`.

## Development setup

See [Development](#development) below. [AGENTS.md](AGENTS.md) is the developer guide. It covers the layout of the code, the feature units and the contracts between them, the registration macros and the rules for working with Bedrock internals. Please read it before changing anything under `src/`.

## Pull requests

1. Fork the repository and create a branch from `main`.
2. Keep each pull request focused on one change. Describe what it does and how you tested it.
3. Follow the existing style: C++20, the provided `.clang-format` and `.editorconfig`, English in code and comments.
4. User-facing text goes through `lo::tr` / `lo::trFor` / `lo::client::ctr` with entries in **both** `assets/lang/en_US/` and `assets/lang/zh_CN/`.
5. Logic in `src/core` must stay free of game headers and come with unit tests in `tests/`:
   `xmake build -y lo-tests; xmake run lo-tests`.
6. Gameplay changes come with selftests (`LO_REGISTER_SELFTEST`) and an end-to-end scenario in [tools/e2e](tools/e2e/README.md). Run the relevant scenarios, and `tools/e2e/server/run.ps1 -Scenario full` for anything that touches shared code.
7. Both platforms must build: `pwsh tools/build.ps1 -Target all -Tests`. CI must pass before a pull request is merged.
8. Never copy code or assets from the Java Carry On repository. It is a behaviour reference only.

Commit messages use the imperative mood ("Add the biome search cooldown", not "Added …"). Update [CHANGELOG.md](CHANGELOG.md) under *Unreleased* for user-visible changes.

## Development

### Building

You need Windows x64, [xmake](https://xmake.io), Visual Studio Build Tools with MSVC 14.4x or newer, and LLVM (`clang-cl`).

```powershell
git config --global core.longpaths true
pwsh tools/build.ps1 -Target all -Tests     # bin/server/LiftOff/ and bin/client/LiftOff/, plus unit tests
pwsh tools/build.ps1 -Target client         # one platform only
```

One code base builds both the dedicated-server mod and the client mod (`--target_type=server|client`). The mod version comes from the latest `v*` git tag.

### Tests

- Unit tests (`src/core`): `xmake build -y lo-tests; xmake run lo-tests`.
- In-game selftests: `/carryon selftest ["glob"]` (operator or server console).
- End-to-end: [tools/e2e](tools/e2e/README.md) runs the real server and client, headless through [Be.D.D.](https://github.com/LiteLDev/BeDD). A `showcase` client scenario re-shoots the screenshots and videos in `docs/showcase/` (listed in `showcase.json` for the LeviMC Mods gallery); encode its PNGs and frames with ffmpeg (webp for images, mp4 + animated webp for clips).

### How it works

- [AGENTS.md](AGENTS.md) is the developer guide: the layout of the code, the feature units and the contracts between them, the registration macros and the rules for working with Bedrock internals. Please read it before changing anything under `src/`.
- [docs/SPEC.md](docs/SPEC.md) describes the Carry On behaviour LiftOff reproduces (blocks, items, mechanics, every screen, configuration, commands, strings).
- LiftOff copies no code or assets from Carry On. Its textures are original pixel art drawn by `tools/textures/*.ps1`.

### Maintainers

[@futrime (Zijian Zhang)](https://github.com/futrime)

## Releasing (maintainers)

1. In [CHANGELOG.md](CHANGELOG.md), move the *Unreleased* entries under a new `## [x.y.z] - YYYY-MM-DD` section and update the links at the bottom. Set `version` in [tooth.json](tooth.json) to `x.y.z`.
2. Merge that into `main`. Then run every server and client e2e scenario against `main`.
3. Tag the merge commit `vx.y.z` and push the tag. The *Release* workflow:
   - builds both platforms and runs the unit tests;
   - checks that the mod manifest and `tooth.json` carry the tag's version;
   - publishes `liftoff-server-windows-x64.zip` and `liftoff-client-windows-x64.zip`, with that version's CHANGELOG section as the release notes.

## License

By contributing you agree that your contributions are licensed under the [LGPL-3.0-or-later](COPYING.LESSER), the license of this project.
