# LiftOff behaviour spec

What LiftOff reproduces from the Java mod [Carry On](https://modrinth.com/mod/carry-on) **2.11.2** (Tschipp).
This is the behaviour reference for every feature unit (see [AGENTS.md](../AGENTS.md)). It describes
behaviour only: never copy code or assets from Carry On.

Conventions in this document:

- **[QUIRK]** marks Carry On behaviour that looks like a bug. Each one carries a **Decision** (reproduce or not, and why).
  Default: reproduce observable behaviour unless it clearly harms Bedrock players.
- **[INF]** marks behaviour inferred from Carry On's code paths rather than stated explicitly. Implement it as written.
- **[LO]** marks a LiftOff decision where Bedrock has no direct equivalent.
- Contract names (`CarryStore`, `Checks`, `Capture`, `Hooks`, `Feedback`, `Slowness`, `Lists`, `PlaceCarried`, `net::...`)
  refer to the spine (U0) headers under `src/feature/carry/`, `src/core/` and `src/net/`.
- "Carrier" = the player who carries; "carry" = the player's `CarryData` record.
- Volumes/pitches are Java values; amplifiers are 0-based (0 = Slowness I).

## 1. Overview

LiftOff lets a player pick up a block (with its block-entity contents, e.g. a full chest), a mob or another player,
carry it in both arms and put it down somewhere else. While carrying, the player is slowed, cannot switch the hotbar
slot, cannot use items or open containers, and cannot break blocks or attack.

**Server-authoritative.** Every rule (pickup, placement, restrictions, persistence, commands) runs on the server
(BDS, or the integrated server of a local world). The client only (a) reports the state of the Carry key and
(b) renders. A player's carry is one `CarryData` record (type `Invalid|Block|Entity|Player`, block state SNBT,
block-entity SNBT, entity SNBT, carried player UUID, `keyPressed`, active script, locked hotbar slot `selected`,
`tick` = carrier tick count at the last change). It is persisted in the world DB under `lo:carry:<uuid>`
(loaded on join, saved on change, autosave and leave). Every `CarryStore::set` refreshes `selected` to the
current slot and `tick` to the current tick. **Tick guard**: no pickup/placement/stacking action is taken in the
same carrier tick as the last carry change (`CarryStore::canAct`), so a pickup and a placement can never happen on
the same click.

**Clients on BDS.**

| | Modded client (LiftOff client build) | Vanilla client |
| --- | --- | --- |
| Detection | Sends `HelloPacket` on join, server answers `WelcomePacket`; `net::isModded(player)`. | No hello. Never receives a LiftOff packet. |
| Carry key | Remappable "Carry" key (default Left Shift), state sent with `KeyPressedPacket` on every change. | Carry key = sneaking (`isSneaking()`), so pickup = sneak + right-click with empty hands. |
| Carried object | Native rendering (§4), first and third person, exactly like Carry On. | Stand-in visual sent by the server (client-only display actors, §4.5) + arm pose from the resource pack. |
| Arm pose | Resource-pack animation with exact values set natively. | Same animation, started by the server (`AnimateEntityPacket`), values quantised. |
| Hotbar lock / containers | Blocked locally (U9) and enforced by the server. | Enforced by the server (slot snapped back, container closed). |

Both kinds of client see every other player's carry: modded clients render it natively from `CarrySyncPacket`,
vanilla clients see the stand-in.

**Local worlds.** The client build contains the integrated server: rules run there, the local player is always
modded (hello/welcome over the local connection), rendering is native.

## 2. Blocks and items

LiftOff adds **no** blocks, items, recipes or loot tables (Carry On has none). The behaviour pack stays empty of
blocks/items. U13 may add resource-pack-only content (player animations, animation controllers, render
controllers, textures) for the arm pose and the stand-in visual; no BP block, item or entity definitions, and no
override of `player.json` in the BP.

## 3. Mechanics

### 3.1 Block pickup (U1)

Trigger: use-block (right-click on a block) on the server. Bedrock sends use-block for an empty hand; every
use-block attempt counts, including the repeats sent while the button is held (Java repeats every 4 ticks too).
When the pickup succeeds the interaction is cancelled (the block is not opened/used).

Preconditions checked by `Checks::canStartPickup(player, blockCentre, /*entity*/false)`, in this order (step 1 and 2):

1. **Distance**: `distSq(player feet position, block centre) < maxDistance² × scale²`. `scale` = 1 when
   `scaleDistance` is false; otherwise current block interaction range / base range = 1 in survival, `5.0/4.5` in
   creative (Java's creative +0.5 block range modifier). Default `maxDistance` 2.5.
2. **General**: game mode is not Adventure or Spectator; main hand **and** offhand are empty; not carrying;
   tick guard passes; `keyPressed` is true (modded: Carry key; vanilla: sneaking).

Then, in this order (first failure ends the attempt **silently**: no sound, no message):

3. Capture the block actor NBT if the block has one (`Capture::captureBlock`).
4. If `useScripts`: find the matching script (§3.12, highest priority first). Remember its `override_checks`.
5. **Block lists**: `Lists::isBlockAllowed(block)` (blacklist, or whitelist when `useWhitelistBlocks`). Never overridable.
6. **Double-height** blocks are refused: any block with the `upper_block_bit` state (doors, sunflower, lilac,
   rose_bush, peony, tall_grass, large_fern, small_dripleaf_block, pitcher_plant, pitcher_crop) and
   `minecraft:seagrass` with `sea_grass_type` ≠ `default` (tall seagrass). Never overridable.
7. **Unbreakable** (destroy speed < 0) refused when the player is not in creative and `pickupUnbreakableBlocks` is
   false. Skipped by `override_checks`.
8. **No block entity** refused unless `pickupAllBlocks`. Skipped by `override_checks`. "Has a block entity" means
   the Java notion: Bedrock-only block actors do **not** count. [LO] These blocks are treated as having no block
   entity for this check (their actor NBT is still captured when they are picked up):
   `minecraft:flower_pot`, `minecraft:noteblock`, `minecraft:cauldron`, `minecraft:piston`,
   `minecraft:sticky_piston`, `minecraft:lodestone`, `minecraft:netherreactor`.
9. **Locked** containers refused: the block actor NBT contains a `Lock` key (any value).
   [QUIRK] Carry On refuses on the mere presence of the key, even an empty lock. **Decision**: reproduce (same
   check); vanilla Bedrock containers never have the key, so it only matters for data from other tools.
10. **Custom pickup condition** (§3.10): the first matching `customPickupConditionsBlocks` rule must be met.
11. **Protection** (`Hooks` protection stage): the built-in hook publishes a cancellable LeviLamina
    `PlayerDestroyBlockEvent` for the block (inside `inProtectionCheck()`); cancelled → refused. Registered
    protection hooks may veto too.
12. **Script** (`Hooks` beforePickup): the script's conditions must hold; the script becomes active
    (`activeScript`, `scriptRender`) and its `commandPickup` runs.
13. Store the block state (SNBT `{name, states, version}`). Water is not part of the carried state: Bedrock keeps
    water in the liquid layer, and it stays in the world (like Java forcing `waterlogged=false` and the fluid staying).
    Block actor saved with its id (`Capture`).
14. Remove the block actor, then the block (`Capture::removeCarriedBlock`): a paired chest is unpaired first (the
    other half becomes a single chest and keeps its own items; each half carries only its own inventory), the
    container is emptied without drops, the block becomes air (or water if it held water). Nothing drops.
15. Play the block's **hit** sound at the block (volume 1.0, pitch 0.5, everyone nearby); swing the main hand.
16. Apply Slowness (§3.4) unless the player is in creative and `slownessInCreative` is false.
17. Log `carry: <player> picked up block <id> at <x> <y> <z>`.

Beds are refused by the list entry `minecraft:bed`; doors by step 6 and `minecraft:*_door`.

### 3.2 Entity pickup (U3)

Trigger: interact with an entity (right-click), hooked at `GameMode::$interact`. On success the vanilla interaction
is cancelled. U3 must verify that vanilla clients send the interact transaction for entities without an
interaction (pig, cow) and for players; any gap is reported in the U3 PR.

1. **Distance**: `distSq(player position, entity position) < maxDistance² × scale²`; `scale` = 1, or `5.0/3.0` in
   creative when `scaleDistance` (Java's creative +2.0 entity range modifier).
2. **General** checks as §3.1 step 2.
3. Entity in post-damage invulnerability (Java `invulnerableTime != 0`; Bedrock: hurt/invulnerable time > 0) → refused.
4. Removed / dead entity → refused.
5. **Tamed with an owner** that is not this player → refused. Never overridable.
6. If `useScripts`: matching script; remember `override_checks`.
7. **Entity lists**: `Lists::isEntityAllowed(actor)`. If not allowed: refused unless `override_checks`, or
   (`allowBabies` and the entity is a baby).
8. Only when the player is **not in creative**:
   - Family `monster` (Java `MobCategory.MONSTER`) refused unless `pickupHostileMobs` or `override_checks`.
   - Size: with `relativePlayerScale`, height and width are multiplied by `1 / carrier scale` (carrier scale is 1
     unless an add-on/command changed it); `height > maxEntityHeight` (2.5) or `width > maxEntityWidth` (1.5)
     → refused. Never overridable. Size = the entity's collision box (AABB) height and width.
9. **Custom pickup condition** (`customPickupConditionsEntities`).
10. **Protection** hook stage (entity). Built-in: none on Bedrock (Carry On fires its own Forge event); registered
    hooks may veto.
11. **Script** conditions; script active; (commandPickup runs in step 12/13).
12. **Target is a player** (`minecraft:player`): refused if `pickupPlayers` is false, or the carrier is not in
    creative and the target is in creative. Otherwise: target's passengers ejected, target stops riding;
    `commandPickup`; target force-mounted on the carrier (§3.3); `CarryData{type=Player, player=uuid}`; swing;
    sound `armor.equip_generic` (1.0, 0.5) at the carrier; Slowness amplifier 1 (Slowness II).
    Log `carry: <player> picked up player <name>`.
13. **Other entity**: passengers ejected, stops riding; if it is an animal on a lead the lead drops as an item;
    `commandPickup`; full entity NBT saved (incl. UniqueID) then removed as "unloaded" (not killed, no drops, no
    death sound) via `Capture::captureActor` + `removeCapturedActor`; sound `armor.equip_generic` (1.0, 0.5) at the
    carrier; swing; Slowness (§3.4). Log `carry: <player> picked up entity <type>`.

Creative bypasses only the hostile and size checks (not lists, owner, lock, invulnerability, protection).

### 3.3 Carrying players (U5)

- The carried player rides the carrier (forced link; `SetActorLinkPacket` to everyone; modded clients additionally get
  `StartRidingOtherPacket{mount, rider, true}`). If Bedrock refuses a player-on-player link, U5 emulates the ride
  (carried player kept in place server-side every tick, link sent to clients) — the observable behaviour must match.
- Every tick the carried player is positioned **0.6 blocks forward** (along the carrier's body yaw) and **0.4 up**
  from the carrier's feet. Its head yaw is clamped to ±30° of the carrier's yaw; its body yaw = carrier yaw.
- The carried player can dismount normally (sneak / jump-dismount button). On dismount: the carrier's carry is
  cleared, Slowness removed, and the carrier gets `StartRidingPacket{carried, false}` (modded) / link removal.
- Whenever the carrier's last passenger stops riding (any cause: dismount, teleport, death, logout), the carrier's
  carry is cleared and Slowness removed.
- Carried player dies or disconnects → the carrier is resolved with `PlaceCarried::placeCarried(carrier)` (§3.5).
- Log `carry: <carrier> released player <name>` on every release.

### 3.4 Carry state and restrictions

**Slowness** (`Slowness.h`). Effect Slowness (id 2), duration 100 000 000 ticks, ambient false, particles hidden.
Not applied when the carrier is in creative and `slownessInCreative` is false. Amplifier (cap 4 = Slowness V,
floor 0):

| Carry | Amplifier |
| --- | --- |
| Player | `1` (fixed; multipliers do not apply) |
| Entity | `i = heavyEntities ? (int)(h × w × f²) : 1`; `i = (int)(i × entitySlownessMultiplier)`; `min(i, 4)` — `f = 1 / carrier scale` when `relativePlayerScale`, else 1; `h`,`w` = collision box |
| Block | `i = heavyTiles ? nbtSize / 500 : 1` (integer division); `i = (int)(i × blockSlownessMultiplier)`; `min(i, 4)` — `nbtSize` = length of the SNBT text of the whole carry record (`CarryData::nbtSize()`) |

Worked examples (defaults unless stated; Bedrock collision boxes):

| Carry | Computation | Result |
| --- | --- | --- |
| Pig 0.9 × 0.9 | (int)0.81 = 0 | Slowness I |
| Cow 1.3 × 0.9 | (int)1.17 = 1 | Slowness II |
| Horse 1.6 × 1.4 | (int)2.24 = 2 | Slowness III |
| Cow, `entitySlownessMultiplier` 2.0 | (int)(1 × 2.0) = 2 | Slowness III |
| Pig, carrier scale 0.5 | f = 2: (int)(0.81 × 4) = 3 | Slowness IV |
| Any entity, `heavyEntities` false | 1 | Slowness II |
| Stone block (record ≈ 150 chars) | 150 / 500 = 0 | Slowness I |
| Chest with 27 full stacks (record ≈ 2 400 chars) | 2400 / 500 = 4 | Slowness V |
| Same chest, `blockSlownessMultiplier` 0.5 | (int)(4 × 0.5) = 2 | Slowness III |
| Any block, `heavyTiles` false | 1 | Slowness II |
| Any player | 1 | Slowness II |

On placement and on clear **every** Slowness effect is removed (`removeSlowness`), including Slowness from other
sources. Slowness removed by milk is not re-applied.

**Hotbar lock** (U6 server, U9 client). While carrying, the selected slot is fixed to `carry.selected`. Server: a
slot change to another slot is reverted (`setSelectedSlot` + `PlayerHotbarPacket{selected, select=true}`), and every
tick the slot is set back if it differs. Modded client: number keys, scroll and other slot changes are blocked
locally and the slot is snapped back each client tick.

**Hand stays empty** (U6). No item ever enters the locked slot while carrying: inventory insertion (ground item
pickup, `/give`, ...) treats the selected slot as occupied (items go to other slots; if only the locked slot is
free, the item stays on the ground / is not given). Pick block (`BlockPickRequest` / `ActorPickRequest`) is
cancelled.

**Use item** (U6): every use-item while carrying is cancelled. **Use block** while carrying is cancelled by the
placement units (U2 for blocks, U4 for entities/players), whether placement succeeds or not; the clicked block is
never opened or used.

**Containers**: any container screen (incl. the inventory) opened while carrying is closed right after it opens.
Modded clients close it locally (U9, like Carry On). For vanilla clients the server closes it (U6,
`ContainerClosePacket`, server-side container closed).

**Entity interaction while carrying a block** [INF]: neither U3 nor U4 acts; the vanilla interaction proceeds (a
resulting container/trade screen is then closed by the container rule).

**hitWhileCarrying** (default false; U6): when false, while carrying: block breaking is cancelled (break speed 0;
`PlayerDestroyBlockEvent` cancelled, also in creative) and attacks on entities are cancelled. U6 ignores events
published inside `carry::inProtectionCheck()`.

**dropCarriedWhenHit** (default false; U6): when true, whenever the carrier takes damage (> 0, not cancelled), the
carry is placed at once with `placeCarried` (§3.5).
[QUIRK] On NeoForge Carry On drops the *attacker's* carry instead of the victim's; Fabric drops the victim's.
**Decision**: do not reproduce; drop the damaged carrier's carry (the documented intent and the Fabric behaviour).

**Script loop** (U8): while carrying with an active script, its `commandLoop` runs every server tick.

No special handling (Carry On has none): game-mode change (Adventure/Spectator only prevent new pickups), water,
lava, sprinting, ladders, swimming, gliding (render only), sleeping, offhand.

### 3.5 Death, respawn, logout, dimension (U7)

**Death.** On death the death position and dimension are remembered. At respawn (Java does this in the clone at
respawn time):

- If the `keepInventory` game rule is true **and** the carry is not a player: the player respawns still carrying;
  `selected` and the rest of the record are kept, the carry is re-synced one tick after respawn.
  [QUIRK][INF] Slowness is not re-applied (death clears effects). **Decision**: reproduce.
- Otherwise `placeCarried` runs at the death position/dimension:
  - **Entity**: recreated (same UniqueID) at the death position.
  - **Block**: placement state computed with a fake context (face down, at the player's block position) and the
    §3.6 state rules; every position in the cube −14..+14 on each axis around the player's block position, sorted by
    distance to the player, is tried; the first where the block can survive, the target is replaceable and no
    entity obstructs wins. Fallback: the player's own block position, **overwriting** what is there. Block set with
    neighbour updates (flags 3), block actor restored, **no** protection/place event, no sound.
  - **Player**: the passenger is ejected.
  - Carry cleared, Slowness removed. Log `carry: placed <what> of <player> at <x> <y> <z> (death)`.
- A player who disconnects on the death screen keeps the carry; the rule runs at the next respawn.

**Carried player dies or disconnects** → `placeCarried(carrier)` (ejects, clears).

**Carrier logout** [INF]: the carry is persisted and the carrier still carries it after login (Slowness persists
with the player's effects). A carried *player* is released on logout (vanilla ejects passengers) and the carrier's
carry is cleared by the §3.3 last-passenger rule.

**Dimension change** [INF]: no handler; the carry stays. Passengers are ejected by the dimension change, which
releases a carried player (§3.3).

### 3.6 Block placement (U2)

Trigger: use-block while carrying a block. The Carry key is **not** required. The interaction is always cancelled.
Because a cancelled `PlayerInteractBlockEvent` rolls back block changes, the placement runs on the next server tick
(the carry and tick guard are re-checked then).

1. Carrying a block and the tick guard passes.
2. Target: the clicked position if that block is replaceable (`canBeBuiltOver`), else the neighbour on the clicked face.
3. **Placement state**: `getPlacementBlock` of the carried block for the target with the clicked face and the click
   position = **the player's feet position** (relative to the target, unclamped).
   [QUIRK] Java uses a miss hit at the player's position, so where on the face you click does not affect slab /
   stair / trapdoor halves (only the player's height relative to the target does). **Decision**: reproduce.
   If no state results, or the block type changed, the stored state is used unchanged. Otherwise start from the
   **stored** state and copy from the placement state only:
   - every **direction/axis** state (built-in list below);
   - every state named by a `placementStateExceptions` entry (by state name, for every block that has it);
   then let the block recompute its neighbour-dependent shape (connections) with neighbour updates after placing,
   and handle water: if the target holds a water source and the block can be waterlogged, water is kept in the
   liquid layer; otherwise no water.

   Built-in recomputed states (Bedrock equivalents of Java `Direction`/`Axis` properties):
   `facing_direction`, `minecraft:facing_direction`, `direction`, `minecraft:cardinal_direction`,
   `minecraft:block_face`, `weirdo_direction`, `torch_facing_direction`, `lever_direction`, `pillar_axis`,
   `portal_axis`, `coral_direction`, `coral_fan_direction`, and `hanging` **only** on `minecraft:pointed_dripstone`
   (Java tip direction).
   Kept from the stored state (not direction/axis in Java): `rail_direction`, `orientation` (crafter, jigsaw: Java
   `FrontAndTop`), `minecraft:vertical_half` (slab type), `attachment` (except via exception), `hanging` (other
   blocks), `huge_mushroom_bits`, `vine_direction_bits` / `multi_face_direction_bits` / `ground_sign_direction` /
   `upside_down_bit` (except via exceptions — the defaults name them), all other states.

   **Java floor/wall pairs merged on Bedrock** [LO]: when the recomputed state would switch a torch
   (`torch_facing_direction` `top` ↔ side) or a skull/head (`facing_direction` 1 ↔ 2..5) between floor and wall
   form, the stored state is used unchanged (Java: "different block → stored state").

   **Built-in block-actor rules** [LO] (Java recomputes these through states that Bedrock keeps in the block actor):
   chests (`minecraft:chest`, `minecraft:trapped_chest`, copper chests) re-pair with a matching neighbour after
   placement (translation of `minecraft:chest[type]`); a floor skull/head (`facing_direction` 1) gets its block
   actor `Rotation` recomputed from the player's yaw like a newly placed head (translation of
   `minecraft:creeper_head[rotation]`).
4. **Can place** iff: the block can survive at the target (`mayPlace`), the player may build there (build ability,
   not Adventure without permission), the target is replaceable, and no entity that blocks building (mobs,
   players incl. the placer, boats, minecarts, armor stands; not items, XP orbs, projectiles) intersects the
   block's collision shape. Failure: sound `lava.pop` (0.5, 0.5) at the player, nothing else.
5. **Protection**: the built-in hook publishes a cancellable `PlayerPlacingBlockEvent` (inside
   `inProtectionCheck()`); registered hooks may veto. Vetoed → refused silently.
6. Target outside the dimension's height range → refused silently.
7. `commandPlace` of the active script runs (`Hooks` onPlace).
8. Set the block with updates (`Capture::restoreBlock`): block actor rebuilt from the stored NBT at the **new**
   position (x/y/z rewritten), `BlockActorData` refreshed; neighbours of the block below are updated.
9. Carry cleared. Place sound (1.0, 0.5) heard once by everyone nearby including the placer. Swing. Slowness
   removed. Log `carry: <player> placed block <id> at <x> <y> <z>`.

`Lists::isPlacementException(blockId, stateName)` answers step 3's exception test.

### 3.7 Entity placement (U4)

Trigger: use-block while carrying an entity or a player (cancelled; executed next tick like §3.6).

1. Target: clicked position, or its neighbour on the clicked face if the clicked block is not replaceable; if that
   is still not replaceable → `lava.pop` (0.5, 0.5) at the player, stop.
2. Position = bottom centre of the target block (`x+0.5, y, z+0.5`). No collision check.
3. **Carried player**: carrier ejects passengers, `StartRidingOther{false}` to modded clients / link removed, carry
   cleared, the carried player is teleported to the position. Swing, Slowness removed. No `commandPlace`.
4. **Other entity**: recreated from the stored NBT (UniqueID and all data kept, spawn reason "bucket");
   protection hook (entity placement, mobs only; default no veto) — vetoed → stop silently; `commandPlace`; entity
   added; a mob plays its ambient sound. Swing, carry cleared, Slowness removed.
   Log `carry: <player> placed entity <type> at <x> <y> <z>`.

### 3.8 Entity stacking (U4)

Trigger: interact with an entity while carrying an entity or a player (interaction cancelled).

1. Requires `stackableEntities`. Held entity = the carried entity (spawned from NBT), or the carrier's first
   passenger when carrying a player.
2. From the clicked entity's root vehicle, count the passengers up the first-passenger chain; the count must be
   `< maxEntityStackLimit − 1` (default: < 9, i.e. a column of at most 10). Else `lava.pop` (0.5, 0.5), stop.
3. Top entity = the highest entity of that chain. If it is the held entity → nothing.
4. Top entity must pass the stacking lists (`Lists::isStackingAllowed`); else nothing (silent).
5. If `entitySizeMattersStacking`: held `h × w` must be ≤ top `h × w`; else `lava.pop` (0.5, 0.5), stop.
6. If the top entity is a `minecraft:horse`, it becomes tamed (only reachable in whitelist mode: horses are in the
   default stacking blacklist).
7. If the squared block distance between the clicked entity and the player is < 6: the held entity is spawned
   2.6 blocks above the clicked entity and then teleported to it; else spawned at the clicked entity's position.
8. The held entity starts riding the top entity (not forced). On Bedrock most entities have no seats: U4 makes the
   link work for any vehicle (seat at the vehicle's height, like Java) or emulates it; if riding still fails, the
   entity simply stays placed (Java: a failed `startRiding` leaves it standing).
9. `commandPlace`, swing, carry cleared, sound `mob.horse.leather` (0.5, 1.5), Slowness removed.
   Log `carry: <player> stacked <type> on <type>`.
10. No protection hook.

### 3.9 Sounds

All played through `Feedback`. Every successful pickup, placement or stacking also swings the main hand.

| Event | Java sound | Bedrock | Volume | Pitch | Heard by |
| --- | --- | --- | --- | --- | --- |
| Block pickup | block's hit sound | `LevelSoundEvent::Hit` with the block's runtime id | 1.0 | 0.5 | everyone near the block |
| Entity/player pickup | `item.armor.equip_generic` | `armor.equip_generic` | 1.0 | 0.5 | everyone near the carrier |
| Block placed | block's place sound | `LevelSoundEvent::Place` with the block's runtime id | 1.0 | 0.5 | everyone incl. placer, once |
| Placement/stacking failed | `block.lava.pop` | `lava.pop` | 0.5 | 0.5 | everyone near the player |
| Entity stacked | `entity.horse.saddle` | `mob.horse.leather` | 0.5 | 1.5 | everyone near the player |
| Entity placed | mob ambient | `Actor::playAmbientSound()` | mob's | mob's | everyone |

Known gap (U0 follow-up): `LevelSoundEvent` carries no pitch, so block hit/place currently use the client's
definition pitch instead of 0.5. The target is pitch 0.5 (e.g. `PlaySoundPacket` with `hit.<sound type>` /
the place sound name of the block's sound type).

### 3.10 Custom pickup conditions (U8)

`customPickupConditionsBlocks` entries `id[state=value,...](stage)`, `customPickupConditionsEntities` entries
`id(stage)`. Ids may use wildcards (§5.3). The **first** entry that matches the block (id + listed states) or entity
decides: the pickup is allowed only if the player has that stage. No matching entry → met.
[LO] Game Stages does not exist on Bedrock: a player has stage `<stage>` iff they have the tag
`carryon:stage:<stage>` (`/tag <player> add carryon:stage:<stage>`).

### 3.11 Protection hooks (U0, used by U1–U4)

`Hooks` protection stage, in pickup/placement order as above. Built-in: block pickup publishes
`PlayerDestroyBlockEvent`, block placement publishes `PlayerPlacingBlockEvent` (land-claim mods listening to these
veto by cancelling). Entity pickup and entity placement have no built-in event; protection mods may register
`LO_REGISTER_PICKUP_HOOKS`. Stacking and `placeCarried` (death/command) fire no protection. Our own units must
ignore events seen while `carry::inProtectionCheck()` is true.

### 3.12 Scripts (U8)

Active only when `useScripts` is true. [LO] Files: `config/scripts/**/*.json` in the mod folder (Java: data-pack
`data/<ns>/carryon/scripts/*.json`); script id = path relative to `config/scripts/` without `.json`. Loaded at
start and on `/carryon reload`. A script is **valid** iff it has exactly one of `object.block` / `object.entity`
**and** at least one of `conditions`, `render`, `effects`, or `"override_checks": true`. Invalid files are logged
and skipped. Scripts are sorted by `priority` (integer, default 0), highest first; the first matching script is used.

```json
{
  "priority": 0,
  "override_checks": false,
  "object": {
    "block":  { "name": "minecraft:chest", "hardness": ">=2", "resistance": "<10", "nbt": { } },
    "entity": { "name": "minecraft:cow", "health": ">5", "height": "<=2", "width": "1", "nbt": { } }
  },
  "conditions": {
    "gamestage": "stage_name",
    "advancement": "minecraft:story/mine_stone",
    "xp": ">=5",
    "gamemode": "0",
    "scoreboard": "objective>=3",
    "position": "x=0,y=60,z=0,dx=10,dy=10,dz=10",
    "effects": "minecraft:speed#1,minecraft:haste#0"
  },
  "render": {
    "name_block": "minecraft:stone",
    "name_entity": "minecraft:pig",
    "nbt": { },
    "translation": "x=0,y=0.5,z=0",
    "rotation": "x=0,y=90,z=0",
    "scale": "x=1,y=1,z=1",
    "rotation_left_arm": "x=1.2,y=0,z=0",
    "rotation_right_arm": "x=1.2,y=0,z=0",
    "render_left_arm": true,
    "render_right_arm": true
  },
  "effects": {
    "commandPickup": "say picked up",
    "commandLoop": "effect @s speed 1 0 true",
    "commandPlace": "say placed"
  }
}
```

Object matching (all given fields must match):
- `block.name` / `entity.name`: id, wildcard allowed (§5.3). `hardness` = destroy speed, `resistance` = explosion
  resistance (Bedrock values). `nbt`: partial compare against the block actor NBT (true when the block has no
  block actor) / the entity NBT. `health` = current health; `height`/`width` = collision box.
- Number bounds: `<=n`, `>=n`, `<n`, `>n`, `=n`, `n` (= equal). Doubles.

Conditions (all must hold):
- `gamestage`: player tag `carryon:stage:<value>` [LO].
- `advancement`: always passes [LO] (no advancements on Bedrock; Carry On passes unknown advancements).
- `xp`: player XP **level**, number bound.
- `gamemode`: number bound on the Java game-mode id: survival 0, creative 1, adventure 2, spectator 3.
- `scoreboard`: `objective<op>value` with op one of `<=`, `>=`, `<`, `>`, `=`; the player's score in that objective
  (no score → false).
- `position`: `x=,y=,z=,dx=,dy=,dz=` (all optional, default 0); for each axis the player's block coordinate must be
  in `[base, base + d]`.
  [QUIRK] An axis whose base is 0 is not checked at all. **Decision**: reproduce (observable, harmless).
- `effects`: comma list `effect#amplifier`; the player must have **every** listed effect with amplifier ≥ the given
  one (`#amp` optional, default 0). Effect ids: Bedrock names, with or without `minecraft:`; Java aliases accepted:
  `hero_of_the_village` → `village_hero`, `jump_boost`, `instant_health`, `instant_damage`, `mining_fatigue`,
  `slow_falling` (same names). Effects missing on Bedrock (`luck`, `unluck`, `glowing`, `dolphins_grace`) never match.

`override_checks` skips: block unbreakable check, block-entity-required check, entity list check, hostile check.
It does **not** skip: block list, double-height, lock, owner, size, custom conditions, protection.

Effects: each command runs as `execute as "<player name>" run <command>` from the server command origin (full
permissions); the position is not changed (Bedrock `execute as` keeps the origin position, like Java).

Render: transferred to clients inside the carry (`CarryData::scriptRender`, JSON of the `render` object) instead of a
separate script sync packet [LO]; consumed by U12 (§4.4). `translation`/`rotation`/`scale` strings
`x=..,y=..,z=..` (missing axes: 0 for translation/rotation, 1 for scale; rotation in degrees). Arm rotations in
radians replace the computed values per given axis; `render_*_arm: false` = vanilla arm pose for that arm.

### 3.13 Lists

Block, entity and stacking lists (§5) are compiled at load and on `/carryon reload` (`Lists`). In whitelist mode
(`useWhitelistBlocks` / `useWhitelistEntities` / `useWhitelistStacking`) only whitelisted ids are allowed and the
blacklist is ignored. Entry syntax §5.3.

## 4. Screens

Carry On has no GUI screens except its Cloth Config screen, which is **not ported** (edit the JSON files and run
`/carryon reload`). There is no HUD element, tooltip or chat message; every failed pickup or placement is silent
(except the `lava.pop` sounds of §3.6–3.8). This section specifies the rendering.

### 4.1 What is drawn

- A carried **block** is drawn as the **item** of the block's default state (contents not shown; a chest uses
  the chest item model). Script `render.name_block` replaces the block; a model override (§5.2) can replace it with
  an item or another block state.
- A carried **entity** is drawn as a client-only display actor created from the stored NBT (`render.name_entity` /
  `render.nbt` override type and NBT), at the carrier's position, all rotations zeroed, hurt time 0, no name tag,
  no leash.
- `isChest` = `minecraft:chest`, `minecraft:trapped_chest`, `minecraft:ender_chest`, and every copper chest.
- `renderHeight` = height of the block's shape (0.5 with an item model override); `renderWidth` = width of the
  block's shape (0.8 with an item override), or entity width × 0.9 (when `rotateEntitiesSideways`: `w − 0.35 w²`).

Transforms below are in Carry On's order (each later op applies in the local frame of the earlier ones), lengths in
blocks, angles in degrees unless stated. U10/U11 must reproduce the same on-screen result as Java and verify it
with side-by-side screenshots.

### 4.2 First person (U10, modded clients)

Replaces the empty-hand rendering while carrying.
- **Block**: scale 2.5; translate (0, −0.5, −1); if `facePlayer != isChest(block)`: rotate 180 about Y, then −8 about
  X; else rotate +8 about X. Then the script transform.
- **Entity**: rotate 180 about Y; scale 0.8; translate (0, −height − 0.2, width × 1.3 + 0.1); script transform;
  then rotate 90 about Y if `rotateEntitiesSideways`.

Script transform: rotate X `rx`, Y `ry`, Z `rz`; translate (x, y, z); scale (x, y, z).

### 4.3 Third person (U11, modded clients; every carrier seen by a modded client)

Base transform: scale 0.6; translate (0, 0, −1.35); if sneaking and not flying: translate y −0.4; if swimming or
gliding: translate z +2.5 and rotate 90 about X; then translate (0, −0.5, 0.65).
- **Block**: rotate −180 about Z; base transform; rotate 180 about Y if `facePlayer != isChest`; translate y by
  `−(renderHeight − 1) / 1.2`; script transform (translation z negated for blocks in the front-facing third-person
  view).
- **Entity**: base transform; rotate 180 about X; translate (0, −3.1, −0.65); scale 1.666; scale
  `(10 − min(9.9, h × w)) × 0.08`; translate (0, h/2 − h/4 + 1, max(width − 0.1, 0.7)); if sneaking translate y −0.4;
  if swimming/gliding: rotate −180 about X, translate (0, 0.2 h − 2, −0.5); if `rotateEntitiesSideways` rotate 90
  about Y; script transform.

### 4.4 Model overrides and script render (U12)

U12 implements a `VisualProvider` that turns a carry into the `CarryVisual` used by U10/U11/U13: model overrides
(`carryon_client.json`, first matching entry wins, §5.2) and script render data (`scriptRender`). Script
`render.name_block` / `name_entity` / `nbt` win over model overrides [INF].

### 4.5 Arm pose and vanilla clients (U13)

Arm pose when `renderArms` is true and the carrier is not swimming or gliding, in radians:
`x = 1.0 + (crouching ? 0.2 : 0) + (entity or player carried ? 0.3 : 0)`;
`offset = min((renderWidth − 1) / 1.5, 0.2)`; right arm rotation (−x, +offset, −0.05); left arm (−x, −offset, +0.05).
Script `rotation_left_arm` / `rotation_right_arm` replace the given axes; `render_left_arm` /
`render_right_arm` false → vanilla pose for that arm.

Implementation: a resource-pack player animation (+ controller) that sets the arm bones. Modded clients drive it
with exact values (molang variables set natively; their own `renderArms`). Vanilla clients get it from the server
via `AnimateEntityPacket` (stop expression ends it when the carry ends); variants per carried kind with crouching
read from `query.is_sneaking`, offset quantised to 0.05 rad and clamped to [−0.65, 0.2], server default
`renderArms` = true.

**Stand-in for vanilla clients**: the server sends client-only display actors (never present on the server) to
vanilla clients only: a block display (e.g. a non-falling `falling_block` with the block's runtime id) or an entity of
the carried type built from the NBT, linked to the carrier and placed to approximate the §4.3 third-person
transform. Vanilla carriers in first person see only their arm pose. Modded clients never get stand-ins.

### 4.6 Key binding (U9)

"Carry" key (`key.carry.desc`), remappable in the controls screen (LeviLamina `KeyRegistry`), default Left Shift,
category "Carry On" where categories exist. Every client tick, a change of the sent state goes to the server
(`KeyPressedPacket`). Sent state = Carry key down; if the key is unbound, or the active input is a gamepad/touch
[LO], the sneak state is sent instead.

## 5. Configuration

### 5.1 `config/carryon.json` (server and integrated server)

Registered as one section (`LO_REGISTER_CONFIG`), file `config/carryon.json`, `version` 1. Values out of range are
clamped on load. Reloaded by `/carryon reload`.

`settings`:

| Key | Default | Range | Meaning |
| --- | --- | --- | --- |
| `maxDistance` | 2.5 | ≥ 0 | Max pickup distance (§3.1/3.2). |
| `maxEntityWidth` | 1.5 | 0–10 | Max entity width (survival). |
| `maxEntityHeight` | 2.5 | 0–10 | Max entity height (survival). |
| `blockSlownessMultiplier` | 1.0 | ≥ 0 | Block Slowness multiplier. |
| `entitySlownessMultiplier` | 1.0 | ≥ 0 | Entity Slowness multiplier. |
| `maxEntityStackLimit` | 10 | ≥ 1 | Max entities in a stacked column. |
| `heavyTiles` | true | | Block Slowness from NBT size. |
| `pickupAllBlocks` | false | | Allow blocks without a block entity. |
| `slownessInCreative` | true | | Apply Slowness in creative. |
| `pickupHostileMobs` | false | | Allow monsters in survival. |
| `heavyEntities` | true | | Entity Slowness from size. |
| `allowBabies` | false | | Babies bypass the entity lists. |
| `useWhitelistBlocks` | false | | Block whitelist mode. |
| `useWhitelistEntities` | false | | Entity whitelist mode. |
| `useWhitelistStacking` | false | | Stacking whitelist mode. |
| `hitWhileCarrying` | false | | Allow breaking/attacking while carrying. |
| `dropCarriedWhenHit` | false | | Place the carry when damaged. |
| `useScripts` | false | | Enable scripts. |
| `stackableEntities` | true | | Enable entity stacking. |
| `entitySizeMattersStacking` | true | | Held entity must not be bigger than the top one. |
| `pickupPlayers` | true | | Allow carrying players. |
| `pickupUnbreakableBlocks` | false | | Allow unbreakable blocks in survival. |
| `scaleDistance` | true | | Scale `maxDistance` with the interaction range. |
| `relativePlayerScale` | true | | Scale entity sizes by the carrier's scale. |
| `placementStateExceptions` | list below | | Extra states recomputed on placement. |

`whitelist`: `allowedEntities`, `allowedBlocks`, `allowedStacking` — all `[]`.
`blacklist`: `forbiddenTiles`, `forbiddenEntities`, `forbiddenStacking` — lists below.
`customPickupConditions`: `customPickupConditionsBlocks`, `customPickupConditionsEntities` — both `[]` (§3.10).

Default `placementStateExceptions` (Java → Bedrock; an entry recomputes the named states on every block that has them):

```json
["minecraft:stone_button[facing_direction]", "minecraft:grindstone[attachment]", "minecraft:vine[vine_direction_bits]", "minecraft:glow_lichen[multi_face_direction_bits]", "minecraft:standing_sign[ground_sign_direction]", "minecraft:trapdoor[upside_down_bit]"]
```

| Java entry | Bedrock |
| --- | --- |
| `minecraft:chest[type]` | No state on Bedrock (pairing lives in the block actor): built-in chest re-pairing (§3.6). |
| `minecraft:stone_button[face]` | Java `face` is shared by buttons, lever and grindstone. Buttons: `facing_direction` (also built-in); lever: `lever_direction` (built-in); grindstone: `attachment` (new entry; also recomputes the bell's `attachment`). |
| `minecraft:vine[north,east,south,west,up]` | `vine_direction_bits` (vine only; Java's shared fence/mushroom booleans have no Bedrock state, `huge_mushroom_bits` kept). |
| `minecraft:creeper_head[rotation]` | Skull rotation lives in the block actor: built-in head rotation rule (§3.6). |
| `minecraft:glow_lichen[...]` | `multi_face_direction_bits` (glow lichen, sculk vein, resin clump — same sharing as Java). |
| `minecraft:oak_sign[rotation]` | `ground_sign_direction` (standing signs, standing banners, hanging signs — same sharing as Java). |
| `minecraft:oak_trapdoor[half]` | `upside_down_bit` (trapdoors and stairs — same sharing as Java's `half`). |

Default `forbiddenTiles`:

```json
["minecraft:end_portal", "minecraft:piston_arm_collision", "minecraft:sticky_piston_arm_collision", "minecraft:end_gateway", "minecraft:tall_grass", "minecraft:large_fern", "minecraft:peony", "minecraft:rose_bush", "minecraft:lilac", "minecraft:sunflower", "minecraft:bed", "minecraft:*_door", "minecraft:big_dripleaf[big_dripleaf_head=false]", "minecraft:waterlily", "minecraft:cake", "minecraft:portal", "minecraft:seagrass[sea_grass_type=double_top]", "minecraft:seagrass[sea_grass_type=double_bot]", "minecraft:frame", "minecraft:glow_frame", "animania:block_trough", "animania:block_invisiblock", "colossalchests:*", "ic2:*", "bigreactors:*", "forestry:*", "tconstruct:*", "rustic:*", "botania:*", "astralsorcery:*", "quark:colored_bed_*", "immersiveengineering:*", "embers:block_furnace", "embers:ember_bore", "embers:ember_activator", "embers:mixer", "embers:heat_coil", "embers:large_tank", "embers:crystal_cell", "embers:alchemy_pedestal", "embers:boiler", "embers:combustor", "embers:catalzyer", "embers:field_chart", "embers:inferno_forge", "storagedrawers:framingtable", "skyresources:*", "lootbags:*", "exsartagine:*", "aquamunda:tank", "opencomputers:*", "malisisdoors:*", "industrialforegoing:*", "minecolonies:*", "thaumcraft:pillar*", "thaumcraft:infernal_furnace", "thaumcraft:placeholder*", "thaumcraft:infusion_matrix", "thaumcraft:golem_builder", "thaumcraft:thaumatorium*", "magneticraft:oil_heater", "magneticraft:solar_panel", "magneticraft:steam_engine", "magneticraft:shelving_unit", "magneticraft:grinder", "magneticraft:sieve", "magneticraft:solar_tower", "magneticraft:solar_mirror", "magneticraft:container", "magneticraft:pumpjack", "magneticraft:refinery", "magneticraft:hydraulic_press", "magneticraft:multiblock_gap", "refinedstorage:*", "mcmultipart:*", "enderstorage:*", "betterstorage:*", "practicallogistics2:*", "wearablebackpacks:*", "rftools:screen", "rftools:creative_screen", "create:*", "magic_doorknob:*", "iceandfire:*", "ftbquests:*", "waystones:*", "contact:*", "framedblocks:*", "securitycraft:*", "forgemultipartcbe:*", "integrateddynamics:cable", "mekanismgenerators:wind_generator", "cookingforblockheads:cabinet", "cookingforblockheads:corner", "cookingforblockheads:counter", "cookingforblockheads:oven", "cookingforblockheads:toaster", "cookingforblockheads:milk_jar", "cookingforblockheads:cow_jar", "cookingforblockheads:fruit_basket", "cookingforblockheads:cooking_table", "cookingforblockheads:fridge", "cookingforblockheads:sink", "chipped:*", "irons_spellbooks:*", "create*:*", "simple_pipes:*", "libmultipart:*", "quark:tiny_potato", "ait:*", "vampirism:*", "extrastorage:*", "relics:researching_table", "sophisticatedstorage:*chest", "powah:*", "advancementtrophies:trophy", "mekanismgenerators:heat_generator", "mna:filler_block", "create_enchantment_industry:*", "graveyard:*", "immersivepetroleum:*", "tardis:interior_door", "cuffed:*", "littletiles:*", "butchersdelight:*", "extended_drawers:*", "functionalstorage:*", "sophisticatedstorage:*", "farmersdelight:*", "modern_industrialization:*_cable", "modern_industrialization:item_pipe", "modern_industrialization:*_item_pipe", "modern_industrialization:fluid_pipe", "modern_industrialization:*_fluid_pipe", "yigd:*", "domum_ornamentum:*", "connectiblechains:*", "cobblemon:*", "snowrealmagic:*", "forbidden_arcanus:black_hole", "sgjourney:*", "copycats:*", "expandedstorage:*", "replication:*", "coloreditemframes:*"]
```

Translation of the vanilla / tag entries:

| Java entry | Bedrock entry | Note |
| --- | --- | --- |
| `#forge:immovable`, `#forge:relocation_not_supported`, `#neoforge:immovable`, `#neoforge:relocation_not_supported`, `#c:relocation_not_supported` | dropped | Loader tags, empty in vanilla; Bedrock has no counterpart. |
| `minecraft:end_portal`, `end_gateway`, `cake` | same | |
| `minecraft:piston_head` | `minecraft:piston_arm_collision`, `minecraft:sticky_piston_arm_collision` | |
| `tall_grass`, `large_fern`, `peony`, `rose_bush`, `lilac`, `sunflower` | same ids | Flattened double plants on Bedrock. |
| `minecraft:*_bed` | `minecraft:bed` | One bed block (colour in the block actor). |
| `minecraft:*_door` | same | Matches `wooden_door`, `iron_door`, `spruce_door`, ... (not trapdoors). |
| `minecraft:big_dripleaf_stem` | `minecraft:big_dripleaf[big_dripleaf_head=false]` | Stem is a state on Bedrock (state filter, §5.3). |
| `minecraft:waterlily` | same | [QUIRK] Dead entry on Java (the id is `lily_pad`), so lily pads are pickupable there; on Bedrock the id exists. **Decision**: keep the entry (intent of the list); lily pads are refused. |
| `minecraft:nether_portal` | `minecraft:portal` | |
| `minecraft:tall_seagrass` | `minecraft:seagrass[sea_grass_type=double_top]`, `[...=double_bot]` | Also refused by the double-height rule. |
| (Java item frame entities) | `minecraft:frame`, `minecraft:glow_frame` | Item frames are blocks on Bedrock. |

Default `forbiddenEntities`:

```json
["minecraft:ender_crystal", "minecraft:ender_dragon", "minecraft:ghast", "minecraft:fireball", "minecraft:small_fireball", "minecraft:wither_skull", "minecraft:shulker", "minecraft:leash_knot", "minecraft:armor_stand", "minecraft:wither_skull_dangerous", "minecraft:dragon_fireball", "minecraft:painting", "minecraft:shulker_bullet", "minecraft:evocation_fang", "minecraft:tnt", "minecraft:thrown_trident", "minecraft:arrow", "animania:hamster", "animania:ferret*", "animania:hedgehog*", "animania:cart", "animania:wagon", "mynko:*", "pixelmon:*", "mocreatures:*", "quark:totem", "vehicle:*", "securitycraft:*", "taterzens:npc", "easy_npc:*", "bodiesbodies:dead_body", "littletiles:*", "connectiblechains:*", "cobblemon:*", "create:*", "swem:*", "toms_mobs:*", "simulated:*", "aeronautics:*", "offroad:*", "coloreditemframes:*"]
```

| Java entry | Bedrock entry | Note |
| --- | --- | --- |
| `#c:capturing_not_supported`, `#c:teleporting_not_supported` | dropped | Loader tags; no Bedrock counterpart. |
| `minecraft:end_crystal` | `minecraft:ender_crystal` | |
| `minecraft:whither_skull`, `minecraft:whither_skull_dangerous` | `minecraft:wither_skull`, `minecraft:wither_skull_dangerous` | [QUIRK] Misspelled, dead on Java. **Decision**: map to the real ids (intent). |
| `minecraft:evoker_fangs` | `minecraft:evocation_fang` | |
| `minecraft:trident` | `minecraft:thrown_trident` | |
| `minecraft:tnt` | same | Primed TNT entity. |
| `minecraft:item_frame`, `minecraft:glow_item_frame` | moved to `forbiddenTiles` (`frame`, `glow_frame`) | |
| `minecraft:spectral_arrow`, `interaction`, `marker`, `block_display`, `item_display`, `text_display` | dropped | No Bedrock entity. |
| other `minecraft:` ids | same | |

Default `forbiddenStacking`:

```json
["minecraft:horse", "minecraft:ender_dragon"]
```

Modded (`animania:`, `create:` ...) ids are kept verbatim (they match nothing on vanilla Bedrock but keep the
list identical for add-ons that reuse those ids).

### 5.2 `config/carryon_client.json` (client build only)

| Section | Key | Default | Meaning |
| --- | --- | --- | --- |
| `settings` | `facePlayer` | false | Carried blocks face the player (chests the other way, §4). |
| `settings` | `rotateEntitiesSideways` | false | Carried entities rotated 90°. |
| `settings` | `renderArms` | true | Arm pose while carrying. |
| `modelOverrides` | `modelOverrides` | list below | Render replacements. |

Default `modelOverrides`:

```json
["minecraft:redstone_wire->(item)minecraft:redstone", "minecraft:bamboo_sapling->(block)minecraft:bamboo", "minecraft:candle_cake->(block)minecraft:cake"]
```

Entry format: `<block>[state=value,...]{snbt}->(item|block)<target>`. `[...]` and `{...}` optional; `(item|block)`
optional, default `item`; a block target may carry `[state=value,...]`. Matches when the block id matches (wildcards
allowed), every listed state equals, and the SNBT partially matches the block actor NBT. First match wins. An item
target renders that item; a block target renders that block (state) as its item. Invalid entries are logged and
skipped.

### 5.3 List entry syntax

- `ns:id` — exact id.
- Wildcard (`*` anywhere): split on `*`; every piece must occur in the id **in order** as a substring
  (`minecraft:*_door` matches `minecraft:wooden_door`; `create*:*` matches every id starting with `create`).
- `#tag` — blocks: Bedrock block tags (`#minecraft:is_pickaxe_item_destructible`, `#wood`); entities: Bedrock type
  families (`#monster`, `#undead`).
- `id[state=value,...]` — [LO] blocks only: also requires the listed states to have those values (used to express
  Java blocks that are states on Bedrock). Without `=value` (placement exceptions, model overrides) a name lists a state.
- Carry On also merges the data-pack tags `carryon:block_blacklist`, `entity_blacklist`, `stacking_blacklist` and the
  three whitelist tags; Bedrock has no data-pack tags, the config lists are the only source.

## 6. Commands

Root `/carryon`, alias `/liftoff`, no permission on the root. Owner U7 (except reload/selftest, U0).

| Command | Permission | Behaviour |
| --- | --- | --- |
| `/carryon debug` | any player | Prints the caller's carry, one line each (also logged). Block: `Block: <id>`, `BlockState: <state SNBT>`, `NBT: <block actor SNBT>`, then `Override Model: <target>` if a model override matches, `Custom Pickup Condition: <entry>` if one matches. Entity: `Entity: <type id>`, `Entity Name: <name tag or type name>`, `NBT: <entity SNBT>`, condition line. Player: `Carrying Player.` Not carrying [INF]: no output. |
| `/carryon clear` | any player | Deletes the caller's carry without placing it, removes Slowness. Carrying a player: the passenger is also dismounted [INF][LO] (no rider stuck on a non-carrier). Message `Cleared 1 Item!`. |
| `/carryon clear <targets>` | op (2) | Same for every target. Message `Cleared N Item!` if N = 1 else `Cleared N Items!`. |
| `/carryon place` | op (2) | Places the caller's carry with `placeCarried` (§3.5). `Placed 1 Item!` |
| `/carryon place <targets>` | op (2) | Same for every target. `Placed N Item!` / `Placed N Items!`. |
| `/carryon reload` | op / console | Existing: reloads configs (incl. `carryon.json`, `carryon_client.json` on the client, scripts) and translations. |
| `/carryon selftest ["glob"]` | op / console | Existing selftest runner. |

N counts every target player, carrying or not. The no-target forms need a player source (the console gets the
framework's player-required error). Messages are plain command output to the source and logged; Java's broadcast to
other ops has no Bedrock equivalent [LO]. Log `carry: cleared <player>` / `carry: placed carried <what> of <player>
at <x> <y> <z> (command)`.

The `Override Model:` and `Custom Pickup Condition:` lines come from U12 and U8: U7 provides a small provider
registry for debug lines (`feature/carry/DebugLines.h`, owned by U7, extension of the contracts); U8 and U12
register their providers once U7 has merged. Without a provider the lines are omitted. On BDS there is no client
config, so `Override Model:` never appears there (as on a Java dedicated server).

## 7. Strings

Carry On keys (en_US text = Carry On's wording). The config-screen keys are defined for completeness (no screen uses
them).

| Key | English | zh_CN | Owner | Where |
| --- | --- | --- | --- | --- |
| `carryon.category.settings` | Settings | 设置 | U0 | lang JSON |
| `carryon.category.blacklist` | Blacklist | 黑名单 | U0 | lang JSON |
| `carryon.category.whitelist` | Whitelist | 白名单 | U0 | lang JSON |
| `carryon.category.modeloverrides` | Model Overrides (Advanced) | 模型覆盖（高级） | U0 | lang JSON |
| `carryon.category.custompickupconditions` | Custom Pickup Conditions (Advanced) | 自定义拾取条件（高级） | U0 | lang JSON |
| `carryon.general.modeloverrides.modeloverrides` | Model Overrides | 模型覆盖 | U0 | lang JSON |
| `carryon.general.blacklist.forbiddenentities` | Entities that the Player cannot pick up | 玩家无法拾起的实体 | U0 | lang JSON |
| `carryon.general.blacklist.forbiddentiles` | Blocks that the Player cannot pick up | 玩家无法拾起的方块 | U0 | lang JSON |
| `carryon.category.custompickupconditions.custompickupconditionsblocks` | Custom Block Pickup Conditions | 自定义方块拾取条件 | U0 | lang JSON |
| `carryon.category.custompickupconditions.custompickupconditionsentities` | Custom Entity Pickup Conditions | 自定义实体拾取条件 | U0 | lang JSON |
| `key.carry.desc` | Carry | 搬运 | U9 | RP `texts.d/<locale>/clientinput.lang` (controls screen uses vanilla I18n) and lang JSON |
| `key.category.carryon.key.carry.category` | Carry On | Carry On | U9 | same as above |

LiftOff keys (`{}` = argument):

| Key | English | zh_CN | Owner |
| --- | --- | --- | --- |
| `lo.carry.cleared` | Cleared {} Item! | 已清除 {} 个物品！ | U7 |
| `lo.carry.cleared_plural` | Cleared {} Items! | 已清除 {} 个物品！ | U7 |
| `lo.carry.placed` | Placed {} Item! | 已放置 {} 个物品！ | U7 |
| `lo.carry.placed_plural` | Placed {} Items! | 已放置 {} 个物品！ | U7 |
| `lo.carry.debug.block` | Block: {} | 方块：{} | U7 |
| `lo.carry.debug.blockstate` | BlockState: {} | 方块状态：{} | U7 |
| `lo.carry.debug.nbt` | NBT: {} | NBT：{} | U7 |
| `lo.carry.debug.override` | Override Model: {} | 覆盖模型：{} | U7 |
| `lo.carry.debug.condition` | Custom Pickup Condition: {} | 自定义拾取条件：{} | U7 |
| `lo.carry.debug.entity` | Entity: {} | 实体：{} | U7 |
| `lo.carry.debug.entity_name` | Entity Name: {} | 实体名称：{} | U7 |
| `lo.carry.debug.player` | Carrying Player. | 正在搬运玩家。 | U7 |

Log lines (`carry: ...`, §3) are English only and not translated.

## 8. Not ported

| Carry On feature | LiftOff |
| --- | --- |
| Cloth Config screen | Not ported: JSON files + `/carryon reload`. |
| Game Stages (custom pickup conditions, script `gamestage`) | Player tags `carryon:stage:<name>`. |
| Advancements (script `advancement`) | Always passes. |
| `Lock` NBT check | Kept; never true on vanilla Bedrock (§3.1 step 9). |
| Data-pack tags `carryon:block_blacklist`, `entity_blacklist`, `stacking_blacklist`, `block_whitelist`, `entity_whitelist`, `stacking_whitelist` | Not available; config lists only. |
| Loader tags `#forge:` / `#neoforge:` / `#c:` | Dropped from the defaults (§5.1). `#tag` entries use Bedrock block tags / families. |
| Data-pack scripts | `config/scripts/**/*.json` (§3.12). |
| `sync_scripts` packet | Render data travels inside the carry (`scriptRender`). |
| First-person body mods compatibility (render skipped with `firstperson`/`firstpersonmod`) | No such mods on Bedrock; not ported. |
| Forge/NeoForge protection events (break/place/`EntityPickupEvent`) | LeviLamina `PlayerDestroyBlockEvent` / `PlayerPlacingBlockEvent` + `Hooks` registry (§3.11). |
| Entities without a Bedrock counterpart (`spectral_arrow`, `marker`, `interaction`, displays) | Dropped from the lists. |
| Java player `SCALE` attribute | Carrier scale (normally 1). |
| Broadcast of command feedback to ops | Output to the source + log. |
| Mod-loader event quirks | NeoForge `dropCarriedWhenHit` attacker quirk not reproduced (§3.4); all other [QUIRK]s reproduced. |

Summary of [QUIRK] decisions: `Lock` presence check — reproduce; player-position hit for placement — reproduce;
script `position` axis with base 0 ignored — reproduce; Slowness not re-applied after keepInventory respawn —
reproduce; `dropCarriedWhenHit` NeoForge attacker drop — not reproduced; dead list entries `minecraft:waterlily`,
`minecraft:whither_skull*` — mapped to live Bedrock ids.

## 9. Work plan

All units depend only on U0 and are independent of each other. Selftest `dx` ranges per AGENTS.md.

| Unit | Name | dx | Scope | Contracts used |
| --- | --- | --- | --- | --- |
| U0 | spine | 0–99 | Done: `CarryLists`, `CarryMath` (core), `CarryConfig` / `CarryClientConfig`, `CarryData`, `CarryStore`, `Checks`, `Slowness`, `Lists`, `Capture`, `PlaceCarried`, `Hooks` (+ visual registry), `Feedback`, `net::CarryProtocol` + client mirror. Follow-ups: list state filters (§5.3), Java-parity block-actor list (§3.1 step 8), block sound pitch (§3.9). | — |
| U1 | blockpickup | 100–199 | §3.1 | `Checks::canStartPickup`, `Lists`, `Capture`, `Hooks`, `Slowness`, `Feedback`, `CarryStore` |
| U2 | blockplace | 200–299 | §3.6; cancels use-block while carrying a block | `Capture::restoreBlock`, `Hooks` (protection, onPlace), `Feedback`, `Slowness`, `Lists::isPlacementException`, `CarryStore` |
| U3 | entitypickup | 300–399 | §3.2 incl. players (pickup side), hook `GameMode::$interact` | `Checks`, `Lists`, `Capture`, `Hooks`, `Slowness`, `Feedback`, `CarryStore`, `net` |
| U4 | entityplace | 400–499 | §3.7, §3.8; cancels use-block while carrying an entity/player | `Capture::spawnActor`, `Hooks`, `Lists::isStackingAllowed`, `Feedback`, `Slowness`, `CarryStore`, `net` |
| U5 | playercarry | 500–599 | §3.3 riding, positioning, dismount, StartRiding packets | `CarryStore`, `net`, `Slowness`, `PlaceCarried` |
| U6 | restrictions | 600–699 | §3.4 hotbar lock (server), item pickup, pick block, use item, attack/destroy, containers (vanilla clients), `dropCarriedWhenHit` | `CarryStore`, `PlaceCarried`, `Hooks::inProtectionCheck`, `net::isModded` |
| U7 | lifecycle | 700–799 | §3.5, §6 (`debug`, `clear`, `place`), debug-line registry | `CarryStore`, `PlaceCarried`, `Slowness`, `Hooks` |
| U8 | scripts | 800–899 | §3.12, §3.10 (scripts + custom pickup conditions), commandLoop | `Hooks` (overrideChecks, customCondition, beforePickup, onPlace, onLoop), `CarryConfig`, `core::CarryLists` |
| U9 | clientinput | 900–999 | §4.6 key, client hotbar lock, client container close | `net::KeyPressedPacket`, client mirror |
| U10 | firstperson | 1000–1099 | §4.2 | `CarryVisual` / `VisualProvider`, client mirror, `CarryClientConfig` |
| U11 | thirdperson | 1100–1199 | §4.3 | same as U10 |
| U12 | modeloverrides | 1200–1299 | §4.4, §5.2 model overrides + script render | `VisualProvider` registry, `CarryClientConfig` |
| U13 | armpose | 1300–1399 | §4.5 RP arm pose for all clients, stand-in for vanilla clients | `CarryVisual`, `net::isModded`, `CarryStore::onCarryChanged`, RP fragments |

Shared events (each unit acts only on its own case; none relies on another's hook order):

| Game event | Units | Rule |
| --- | --- | --- |
| use-block | U1 (not carrying), U2 (carrying a block), U4 (carrying entity/player) | The tick guard prevents pickup and placement on the same click. U6 does not touch use-block. |
| entity interact (`GameMode::$interact`) | U3 (not carrying), U4 (carrying entity/player: stacking) | Tick guard prevents pickup + stack in one click. Carrying a block: neither acts. |
| block destroy / attack | U6, and U1's protection publish | U6 ignores events while `carry::inProtectionCheck()`. |
| block placing event | U2's protection publish | No unit cancels it for its own use. |
| damage | U6 (`dropCarriedWhenHit`), U7 (death) | U6 skips a carrier that is dying. |
| ride/dismount, logout | U5 (release, last-passenger rule), U7 (carrier/carried player logout and death) | Both call `PlaceCarried`/`CarryStore::clear`, which are idempotent. |
| hotbar slot, containers | U6 (server), U9 (modded client) | Independent; server is authoritative. |
| rendering | U10, U11 draw; U12 supplies visuals; U13 arm pose + vanilla stand-ins | U13 sends stand-ins only to vanilla clients. |
| debug lines | U7 registry; U8, U12 providers | Providers added after U7 merges. |
