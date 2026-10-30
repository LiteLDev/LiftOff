# AGENTS.md

LiftOff is a Bedrock port of the *behaviour* of the Java mod **Carry On 2.11.2** (Tschipp),
written as a LeviLamina native mod. It targets MC **1.26.51** / LeviLamina **26.51.7**, builds with
xmake + clang-cl on Windows x64, and runs on **both** the dedicated server (BDS) and the client
(local worlds through LeviLauncher). One code base, two builds (`--target_type=server|client`).

The behaviour reference is `docs/SPEC.md`. Write it (blocks, items, mechanics, every screen, config,
commands, strings) before the first feature unit, and read it before working on a unit.

Policy: never copy code or assets from the Java Carry On repository (behaviour reference only;
it is licensed LGPL-3.0-only). Textures are original pixel art drawn by `tools/textures/*.ps1`. English UI strings
should match the wording of Carry On (they are behaviour), written into our own lang files.

## Layout

| Path | What it is |
| ---- | ---------- |
| `src/core/` | Pure C++ (no `mc/`/`ll/` includes): `Format.h` (translation formatting), `Glob.h`, `GlobalPos.h`, `CarryLists.h` (list entries), `CarryMath.h` (Slowness, reach). Static lib `lo-core`, unit-tested in `tests/`. |
| `src/game/` | Game adapters: `WorldDb` (LevelDB JSON + world lifecycle), `Blocks` (states, placement, positions), `Items` (`makeItem`). |
| `src/client/` | Client side (client build only, files wrapped in `#ifdef LO_CLIENT`): `Client.h` (client instance, local player, `ctr`). |
| `src/mod/` | Entry (`Main.cpp`), `Log.h`, `Module.h`, `Config.h`, `ModConfig.h`, `I18n.h`, `Commands.h`, `Registry.h`. |
| `src/selftest/` | In-game test framework (`SelfTest.h`) and the foundation tests. |
| `src/pack/` | `PackDeployer` (BDS: puts the resource pack on the world). |
| `src/feature/carry/` | The carry spine (U0): the shared contracts every unit builds on (see *Carry contracts* below). |
| `src/net/` | `CarryProtocol` (U0): client <-> server packets and the client mirror of every carry. |
| `src/feature/<unit>/` | Feature units (gameplay, server side; may contain `#ifdef LO_CLIENT` parts). |
| `assets/lang/<locale>/<unit>.json` | Mod translations (server + client mod text), merged into `lang/<locale>.json`. |
| `assets/pack/bp/` | Behavior pack: `blocks/`, `items/`, `recipes/`, `loot_tables/`, `manifest.json`. |
| `assets/pack/rp/` | Resource pack: `textures/`, `models/`, fragments `item_texture.d/`, `terrain_texture.d/`, `blocks.d/`, `sounds.d/`, `texts.d/<locale>/*.lang`. |
| `tools/build.ps1` | Builds server and/or client into `bin/<target>/LiftOff/`. |
| `tools/xmake/assemble.lua` | Post-build: copies binaries, merges lang + pack fragments. |
| `tools/textures/lib.ps1` | Pixel-art helpers (`Save-PixelArt`, `Merge-Rows`, `Shade-Color`). |
| `tools/e2e/` | Real-game harnesses (server + client); see `tools/e2e/README.md`. |

## Work as a feature unit (no shared-file edits)

Each feature unit owns its own files and registers itself. **Do not edit** `src/mod/Main.cpp`,
existing contracts or other units' files. Extend contracts compatibly (add, never change/remove)
and only when your unit really needs it; call such additions out in the PR.

Files a unit typically adds (`<unit>` = a short lowercase name):
- `src/feature/<unit>/*.cpp|h`, namespace `lo::feature::<unit>`; client parts in `src/client/<unit>/`.
- `assets/lang/en_US/<unit>.json` + `assets/lang/zh_CN/<unit>.json` (both required).
- BP: `assets/pack/bp/blocks/<id>.json`, `items/<id>.json`, `recipes/<id>.json`, `loot_tables/blocks/<id>.json`.
- RP: `textures/blocks|items/*.png` (drawn by `tools/textures/<unit>.ps1`), `models/blocks/*.geo.json`,
  fragments `terrain_texture.d/<unit>.json`, `item_texture.d/<unit>.json`, `blocks.d/<unit>.json`,
  `sounds.d/<unit>.json`, `texts.d/{en_US,zh_CN}/<unit>.lang` (block / item names).
- Selftests in the unit's .cpp (`LO_REGISTER_SELFTEST("<unit>.<case>", ...)`), e2e scenarios in
  `tools/e2e/{server,client}/scenarios/<unit>.ps1`.

Duplicate keys in any merged fragment (lang JSON, item_texture, terrain_texture, blocks, texts,
sounds) fail the build. A translation key belongs to the unit that owns the feature; other units
reference it with `tr(key, "English fallback")` without defining it.

### Registration macros

| Macro | Header | Purpose |
| ----- | ------ | ------- |
| `LO_REGISTER_MODULE(T)` / `LO_REGISTER_MODULE_ORDERED(T, order)` | `mod/Module.h` | `T` has `install()`/`uninstall()` (+ optional `load()`, + optional `static T& get()`). |
| `LO_REGISTER_CONFIG("section", T)` | `mod/Config.h` | Reflectable struct with `int version`; file `config/<section>.json`; `lo::config::get<T>()`. One section per unit (the spine has two: `carryon` and `carryon_client`, Carry On's common and client files). |
| `LO_REGISTER_SUBCOMMAND(fn)` | `mod/Commands.h` | Adds overloads to `/carryon` (alias `/liftoff`). `commands::playerOf`, `commands::isAdmin`. |
| `LO_REGISTER_SELFTEST("unit.case", fn)` | `selftest/SelfTest.h` | `fn(Context&)` or `ll::coro::CoroTask<>(Context&)`. |
| `LO_REGISTER_PICKUP_HOOKS(PickupHooks{...})` | `feature/carry/Hooks.h` | Pickup / placement stages: `overrideChecks`, `customCondition`, `protection`, `beforePickup`, `placeProtection`, `onPlace`, `onLoop` (scripts, custom conditions, land protection). |
| `LO_REGISTER_VISUAL_PROVIDER(VisualProvider{...})` | `feature/carry/Hooks.h` | Changes what is drawn for a carry (`CarryVisual`: block / item / entity, transform, arms). |
| `LO_REGISTER_DEBUG_LINES(fn)` | `feature/carry/Hooks.h` | Extra lines of `/carryon debug`. |

Registrations live in .cpp files linked directly into the DLL, so they are never stripped.

## Contracts in short

- **Ids**: `liftoff:<path>`, with the paths Carry On uses where they exist.
- **World data** (`game/WorldDb.h`): JSON in the world's LevelDB under `lo:<unit>:<...>` keys.
  `db::onWorldReady/onWorldSave/onWorldClosed` for the world lifecycle (the client loads many
  worlds in one process). Never keep `Player*` or block pointers across ticks.
- **Strings**: `lo::tr("key", "English fallback", args...)` / `lo::trFor(player, ...)`, client
  `lo::client::ctr(...)`, `{}`/`{0}` placeholders. Prefer the key names of Carry On for strings
  that exist there; `lo.<unit>.*` for LiftOff-only text. Block / item names come from RP `texts.d`.
- **Logging**: `lo::log()` (`mod/Log.h`). No per-tick logging. Log one line per user-visible action
  that e2e scenarios can wait for.
- **Selftests** (`selftest/SelfTest.h`): `ctx.player()` (SimulatedPlayer), `ctx.spawnPlayer()`,
  `ctx.testPos(dx)` (distinct `dx` per test; unit U<n> owns dx 100n..100n+99; the run keeps the
  area loaded with a ticking area), `ctx.setBlock(pos, id, states)`, `ctx.cleanupBlock`, `ctx.give`,
  `co_await ctx.ticks(n)`, `co_await ctx.ensureLoaded(pos)`, `ctx.check/require/fail/info`.
  `/carryon selftest` (no glob) and the server scenario `full` run every unit's tests together:
  keep them passing there.

## Carry contracts (U0 spine)

Namespace `lo::feature::carry` (headers in `src/feature/carry/`) unless noted; server thread unless noted.
`docs/SPEC.md` says which unit uses what and in which order (§3 check orders, §9 work plan). Each header
has usage examples at the top.

| Header | Key API | Used by |
| ------ | ------- | ------- |
| `CarryConfig.h` | `CarryConfig` (`config/carryon.json`: `settings`, `whitelist`, `blacklist`, `customPickupConditions`), `CarryClientConfig` (`config/carryon_client.json`: `settings`, `modelOverrides`): `lo::config::get<CarryConfig>().settings.maxDistance` | all |
| `CarryData.h` | `CarryData{type, block, blockEntity, entity, player, keyPressed, activeScript, scriptRender, selected, tick}` (NBT as SNBT text), `toJson` / `fromJson`, `clear()`, `nbtSize()` | all |
| `CarryStore.h` | `get(p)`, `isCarrying(p)`, `set(p, data)` (refreshes `selected` + `tick`, persists `lo:carry:<uuid>`, syncs modded clients), `clear(p)`, `tickCount(p)`, `canAct(p)` (tick guard), `keyPressed(p)` / `setKeyPressed` / `resetKeyPressed`, `onCarryChanged(fn(p, old, new))`, `forEachCarrier(fn)` | all |
| `Checks.h` | `canStartPickup(p, target, entity)` (distance with reach scale, game mode, both hands empty, not carrying, tick guard, key), `gameType(p)`, `isCreative(p)`, `isDoubleHeight(block)`, `isUnbreakable(block)`, `isBedrockOnlyBlockActor(id)` | U1, U3 |
| `Lists.h` | `isBlockAllowed(block)`, `isEntityAllowed(actor)`, `isStackingAllowed(actor)`, `isBlockIdAllowed(id)`, `isEntityIdAllowed(id)`, `isPlacementException(state)`, `blockStateValue(block, name)`, `blockCandidate` / `entityCandidate` (match your own `core::carry::ListEntry`s, U8 / U12) | U1–U4, U8, U12 |
| `Slowness.h` | `blockAmplifier(data)`, `entityAmplifier(entity, carrier)`, `playerAmplifier()`, `applySlowness(p, amp)` (honours `slownessInCreative`), `removeSlowness(p)`, `slownessAmplifier(p)`, `entityHeight` / `entityWidth`, `playerScale` | U1, U3–U7 |
| `Capture.h` | `captureBlock(region, pos)` -> `{block, blockEntity}` SNBT, `removeCarriedBlock(region, pos)` (unpairs chests, empties containers, keeps water, no drops), `restoreBlock(region, pos, data, state, waterlog)` (block actor rebuilt at the new position, chests re-paired, sent to clients), `carriedBlock(data)`, `carriedBlockId(data)`, `captureActor(actor)`, `removeCapturedActor(actor)` (no death, no drops, lead dropped), `spawnActor(region, pos, snbt)` (keeps the UniqueID), `entityIdOf(snbt)`, `toSnbt` / `parseNbt` | U1–U4, U7 |
| `PlaceCarried.h` | `placeCarried(p)`, `placeCarriedAt(p, region, pos)` (death / command placement: ±14 cube search, fallback overwrite; clears the carry and removes Slowness), `canPlaceBlockAt(region, pos, block)`, `isObstructed(region, pos, block)` | U2, U5–U7 |
| `Hooks.h` | stage aggregates `overrideChecks(t)`, `customConditions(t)`, `protectionAllows(t)` (publishes `PlayerDestroyBlockEvent`), `beforePickup(t, data)`, `placeAllowed(t, block)` (publishes `PlayerPlacingBlockEvent`), `runOnPlace(p, data)`; `inProtectionCheck()`; `setPlacementStateResolver(fn)` / `resolvePlacementState(...)` (U2 installs the §3.6 rules); `visualFor(data)` -> `CarryVisual`; `extraDebugLines(p, data)` | U1–U4, U6–U8, U10–U13 |
| `Feedback.h` | `playBlockSound(p, pos, block, BlockSound::Hit/Place)`, `playSound(actor or dim + pos, name, volume, pitch)`, `sound::kArmorEquip` / `kLavaPop` / `kHorseSaddle`, `playAmbient(actor)`, `swing(p)` | U1–U4 |
| `net/CarryProtocol.h` (`lo::net`) | `isModded(p)`, `renderArms(p)` (viewer's client setting, sent with the hello), `onModdedJoined(fn)`, `sendCarrySync(carrier)` (automatic on every store change), `sendCarrySyncTo`, `sendStartRiding(to, entity, ride)`, `broadcastStartRidingOther(mount, rider, ride)`; test seams `setTestModded`, `takeTestMessages`, `injectFromClient`. Client build (`lo::client::carry`, client thread): `carriedOf(uniqueId)`, `localData()`, `serverHasMod()`, `sendKeyPressed(bool)`, `onSync`, `onStartRiding`, `onStartRidingOther`, `onConnected` | U3–U6, U9–U13 |
| `core/CarryLists.h`, `core/CarryMath.h` (`lo::core::carry`) | `parseEntry`, `wildcardMatch`, `IdList`, `isAllowed`, `StateExceptions`, `splitSpec` (for `(stage)` / `{nbt}->target` suffixes), `parseStateFilters`; `blockSlowness`, `entitySlowness`, `reachScale`, `withinReach` | U8, U12 (their own formats) |

Rules of the spine:
- Read and write a player's carry only through `CarryStore`; never keep a `CarryData const&` across calls.
- Every pickup / placement goes through the `Hooks` stages in the SPEC order, so scripts and protection
  plugins see every unit the same way. Ignore synthetic events while `inProtectionCheck()`.
- Never send LiftOff packets yourself; use the `net` functions (they skip vanilla clients).
- SimulatedPlayers are vanilla clients: selftests use `carry::setKeyPressed(p, true)` / `resetKeyPressed`,
  and `net::setTestModded` to see the packets a modded client would get.

## Build, test, run

In a fresh pwsh refresh PATH first (the build script does this and adds LLVM if needed):
`$env:Path=[Environment]::GetEnvironmentVariable('Path','Machine')+';'+[Environment]::GetEnvironmentVariable('Path','User')`

```powershell
pwsh tools/build.ps1 -Target all -Tests     # bin/server/LiftOff + bin/client/LiftOff + gtest
pwsh tools/build.ps1 -Target server -Tests  # one platform + unit tests
xmake build -P . -y lo-tests; xmake run -P . lo-tests
pwsh -NoProfile -File tools/e2e/server/run.ps1 -Scenario smoke       # BDS (4 slots, waits for a free one)
pwsh -NoProfile -File tools/e2e/client/run.ps1 -Scenario smoke       # headless client via Be.D.D.
```

- `git config --global core.longpaths true` is required (long LeviLamina header paths).
- `clang-cl` must be on PATH (`C:\Program Files\LLVM\bin`).
- `xmake-requires.lock` is committed; it holds entries for both target types.
- In game: `/carryon selftest ["glob"]` (op or server console). Quote globs: `carryon selftest "<unit>.*"`.
  Output lines: `SELFTEST PASS <name>`, `SELFTEST FAIL <name>: <reason>`, `SELFTEST DONE pass=<N> fail=<M>`.
- Log contract: `LiftOff enabled` on enable; `LiftOff world ready` when a world is available.
- E2E client runs: set `$env:LEVI_E2E_SLOTS='1,2,3,4'` to take the first free game slot. Locks live in
  `%APPDATA%\levilauncher.exe\` (`bei-e2e.lock` for slot 1, `hiN-e2e.lock` for clones); the harness
  waits for them. Headless input (Be.D.D.) is the default: the game runs in the
  background and never takes the real mouse/keyboard.

Deploying by hand: copy `bin/server/LiftOff` to `<BDS>/plugins/`, or `bin/client/LiftOff` to
`%APPDATA%\levilauncher.exe\versions\1.26.51.01\mods\`.

## Bedrock / LeviLamina rules (learned the hard way)

- Headers: `%LOCALAPPDATA%\.xmake\packages\l\levilamina\26.51.7\<hash>\include\{ll,mc}` (one hash per
  target type). Check them before guessing a name or signature.
- Callable: `MCAPI`, `MCFOLD`, `MCTAPI`, unmarked `virtual`. **Never** `MCNAPI`. `ll::TypedStorage`
  members are fields (`.get()`, `*`, `->`). Packets without an MCAPI constructor are created with
  `MinecraftPackets::createPacket(MinecraftPacketIds::X)` + `static_cast`; send with `pkt.sendTo(player)`.
- Hook virtuals via `&Cls::$fn` with `LL_TYPE_INSTANCE_HOOK`; `hook()` in `install()`, `unhook()` in `uninstall()`.
- In `ServerNetworkHandler::$handle` hooks `this` is the `NetEventCallback` base subobject: use
  `thisFor<NetEventCallback>()->_getServerPlayer(source, packet.mSenderSubId)`.
- **No `dynamic_cast` on game types** (no usable RTTI). A Player that is not `isClientSide()` is a `ServerPlayer`.
- Types used as command parameters must have external linkage (not in an anonymous namespace).
- `ItemStack` has no `(name, count, aux)` constructor: use `game::makeItem(id, count, aux)`.
- BDS enables mods **after** "Server started" (the world already exists at `install()`); the client
  enables mods at game start (no world yet) and fires `ServerStartedEvent` for every local world.
  Write modules that work in both orders (`game::db::onWorldReady`).
- `ll::service::getLevel()` on the client is the integrated server's level (null outside a world).
- Behavior packs in `mods/<name>/behavior_packs/` are added to every world by LeviLamina; resource
  packs are added to the client's global stack. On BDS `PackDeployer` adds the RP to the world.
- Custom blocks cannot have block entities: keep per-block state in block states + the world DB.
- A cancelled `PlayerInteractBlockEvent` rolls back block changes made during it: defer world edits
  to the next tick (`ll::coro` / `ServerLevelTickEvent`).
- Don't keep `Player*` across ticks; store `getOrCreateUniqueID()` / the UUID and look it up again.
- `ll::TypedStorage<..., T>` of a trivial type (int, float, bool, pointers) *is* `T`: no `.get()`.
- A Bedrock large chest is one 54-slot container on both halves (`BlockSource::tryGetContainer`): slots 0-26
  live in the lead half, 27-53 in the other; each half's NBT holds its own 27 slots.
- `BlockSource::spawnActor(nbt)` gives the actor a new UniqueID; `carry::spawnActor` keeps it
  (`ActorFactory::loadActor` with `DefaultDataLoadHelper` + `Level::addEntity`).
- `BlockActor::create(nbt, pos)` returns null for vanilla chests; `BlockActor::loadStatic(blockType, ...)` works.
- Worktrees nested in the main checkout: xmake prefers the parent's `.xmake`, so `tools/build.ps1` passes
  `-P <repo>`. Run raw xmake commands with `-P .` too.

## Conventions

- C++20, `.clang-format` (LLVM-based, 4 spaces, 120 columns). Namespaces follow directories
  (`lo::core`, `lo::game`, `lo::feature::<unit>`, `lo::client::...`).
- English in code and comments; user-facing text through `tr`/`trFor`/`ctr` + lang fragments (en_US and zh_CN).
- Every `src/core` change comes with gtest coverage in `tests/`; every feature comes with selftests
  and an e2e scenario.
- Commit in small, buildable steps. Both platforms must build (`pwsh tools/build.ps1 -Target all`).
- Commit / PR titles in the imperative mood, as in [CONTRIBUTING.md](CONTRIBUTING.md) ("Add the biome search cooldown"),
  PR into `main`. Work on a unit of the plan may prefix its title with the unit id (`U<n>: <summary>`).

## Out of scope

See `docs/SPEC.md` §8: the Cloth config screen, Game Stages (player tags `carryon:stage:<name>` instead),
advancements (script condition always passes), data-pack tags, first-person body mod compatibility.
