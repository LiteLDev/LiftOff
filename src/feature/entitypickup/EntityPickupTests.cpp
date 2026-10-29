// Selftests of entity pickup (U3, dx 300..399).

#include <string>
#include <vector>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/Int64Tag.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/entity/components_json_legacy/TameableComponent.h"
#include "mc/entity/utilities/leashable_utility/LeashableUtility.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/entity/components_json_legacy/Interaction.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/phys/AABB.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/CarryStore.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Lists.h"
#include "feature/carry/Slowness.h"
#include "feature/entitypickup/EntityPickup.h"
#include "game/Blocks.h"
#include "game/Items.h"
#include "mod/Config.h"
#include "selftest/SelfTest.h"

namespace {
using lo::selftest::Context;
namespace carry = lo::feature::carry;
namespace ep    = lo::feature::entitypickup;
using ep::Result;

/// Stands the test player at `dx` (feet on the block centre, facing south) and returns that block.
ll::coro::CoroTask<BlockPos> arena(Context& ctx, int dx) {
    auto pos = ctx.testPos(dx);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    BlockPos feet = lo::game::blockPos(pos);
    auto&    p    = ctx.player();
    p.teleport(feet.bottomCenter(), p.getDimensionId());
    co_await ctx.ticks(2);
    co_return feet;
}

/// Spawns `id` one block east of `feet` (inside the reach of 2.5 blocks).
Actor* spawnNear(Context& ctx, BlockPos const& feet, std::string const& id, int east = 1) {
    auto  level = ll::service::getLevel();
    auto& reg   = ctx.player().getDimensionBlockSource();
    Vec3  at    = BlockPos{feet.x + east, feet.y, feet.z}.bottomCenter();
    Actor* a    = level->getSpawner().spawnMob(reg, ActorDefinitionIdentifier(id), nullptr, at, false, false, false);
    ctx.require(a != nullptr, "spawned " + id);
    a->setPersistent();
    return a;
}

/// Puts `a` back at `east` blocks east of `feet` (mobs wander, panic and follow their owner).
void pin(Actor& a, BlockPos const& feet, int east = 1) {
    a.teleport(BlockPos{feet.x + east, feet.y, feet.z}.bottomCenter(), a.getDimensionId());
}

void removeIfAlive(ActorUniqueID id) {
    if (auto level = ll::service::getLevel())
        if (auto* a = level->fetchEntity(id, false)) a->remove();
}

/// Ends a test: drops the carry and Slowness, releases the key, survival again.
void reset(Player& p) {
    carry::clear(p);
    carry::removeSlowness(p);
    carry::resetKeyPressed(p);
    p.setPlayerGameType(GameType::Survival);
}

std::string want(Result got, Result expected) {
    return std::string("expected ") + std::string(ep::resultName(expected)) + ", got " + std::string(ep::resultName(got));
}

/// A protection hook that refuses entity pickups while a test sets it.
bool gRefuseEntities = false;
LO_REGISTER_PICKUP_HOOKS(carry::PickupHooks{
    .name       = "entitypickup-selftest",
    .protection = [](carry::PickupTarget const& t) { return !(gRefuseEntities && t.entity); },
});

/// Restores the carry config after a test changed it.
struct ConfigRestore {
    carry::CarryConfig saved = lo::config::get<carry::CarryConfig>();
    ~ConfigRestore() {
        lo::config::mut<carry::CarryConfig>() = saved;
        carry::rebuildLists();
    }
};
} // namespace

LO_REGISTER_SELFTEST("entitypickup.pig", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p    = ctx.player();
    auto  feet = co_await arena(ctx, 300);
    auto* pig  = spawnNear(ctx, feet, "minecraft:pig");
    pig->setNameTag("lo_ep_pig");
    co_await ctx.ticks(2);
    auto id = pig->getOrCreateUniqueID();

    auto r = ep::tryPickup(p, *pig);
    ctx.check(r == Result::Preconditions, "no pickup without the carry key: " + want(r, Result::Preconditions));
    carry::setKeyPressed(p, true);
    r = ep::tryPickup(p, *pig);
    ctx.require(r == Result::PickedUp, "pig picked up: " + want(r, Result::PickedUp));
    auto data = carry::get(p);
    ctx.check(data.isEntity(), "the carry is an entity");
    ctx.check(carry::entityIdOf(data.entity) == "minecraft:pig", "the carry is the pig");
    auto tag = carry::parseNbt(data.entity);
    ctx.require(tag.has_value(), "entity NBT parses");
    ctx.check((*tag)["UniqueID"].get<Int64Tag>().data == id.rawID, "the UniqueID is stored");
    co_await ctx.ticks(2);
    ctx.check(ll::service::getLevel()->fetchEntity(id, false) == nullptr, "the pig is removed from the world");
    ctx.check(carry::slownessAmplifier(p) == 0, "a pig gives Slowness I (0.9 x 0.9)");

    // The UUID survives a round trip (U4 places it back with carry::spawnActor).
    auto* back = carry::spawnActor(p.getDimensionBlockSource(), BlockPos{feet.x + 2, feet.y, feet.z}.bottomCenter(), data.entity);
    ctx.require(back != nullptr, "the pig is recreated from the carry");
    ctx.check(back->getOrCreateUniqueID() == id, "the recreated pig keeps its UniqueID");
    ctx.check(back->getNameTag() == "lo_ep_pig", "the recreated pig keeps its name");
    back->remove();
    reset(p);
});

LO_REGISTER_SELFTEST("entitypickup.cow_slowness", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p    = ctx.player();
    auto  feet = co_await arena(ctx, 306);
    auto* cow  = spawnNear(ctx, feet, "minecraft:cow");
    co_await ctx.ticks(2);
    int expected = carry::entityAmplifier(*cow, p);
    carry::setKeyPressed(p, true);
    auto r = ep::tryPickup(p, *cow);
    ctx.require(r == Result::PickedUp, "cow picked up: " + want(r, Result::PickedUp));
    co_await ctx.ticks(1);
    ctx.check(expected == 1, "a cow (1.3 x 0.9) computes Slowness II, got " + std::to_string(expected));
    ctx.check(carry::slownessAmplifier(p) == expected, "Slowness of the cow is applied");
    reset(p);
});

LO_REGISTER_SELFTEST("entitypickup.tick_guard_and_hands", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p    = ctx.player();
    auto  feet = co_await arena(ctx, 312);
    auto* a    = spawnNear(ctx, feet, "minecraft:pig");
    auto* b    = spawnNear(ctx, feet, "minecraft:pig", -1);
    auto  bid  = b->getOrCreateUniqueID();
    co_await ctx.ticks(2);
    carry::setKeyPressed(p, true);
    ctx.give(p, lo::game::makeItem("minecraft:stick"), p.getSelectedItemSlot());
    auto r = ep::tryPickup(p, *a);
    ctx.check(r == Result::Preconditions, "a held item prevents the pickup: " + want(r, Result::Preconditions));
    p.getInventory().setItem(p.getSelectedItemSlot(), ItemStack{});
    r = ep::tryPickup(p, *a);
    ctx.require(r == Result::PickedUp, "picked up with empty hands: " + want(r, Result::PickedUp));
    r = ep::tryPickup(p, *b);
    ctx.check(r == Result::Preconditions, "no second pickup while carrying: " + want(r, Result::Preconditions));
    removeIfAlive(bid);
    reset(p);
});

LO_REGISTER_SELFTEST("entitypickup.distance", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p    = ctx.player();
    auto  feet = co_await arena(ctx, 318);
    auto* pig  = spawnNear(ctx, feet, "minecraft:pig", 3);
    auto  id   = pig->getOrCreateUniqueID();
    co_await ctx.ticks(1);
    carry::setKeyPressed(p, true);
    auto r = ep::tryPickup(p, *pig);
    ctx.check(r == Result::Preconditions, "3 blocks is out of reach (2.5): " + want(r, Result::Preconditions));
    removeIfAlive(id);
    reset(p);
});

LO_REGISTER_SELFTEST("entitypickup.babies", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&         p = ctx.player();
    ConfigRestore restore;
    auto&         cfg = lo::config::mut<carry::CarryConfig>();
    cfg.blacklist.forbiddenEntities.push_back("minecraft:pig");
    cfg.settings.allowBabies = false;
    carry::rebuildLists();

    auto  feet  = co_await arena(ctx, 324);
    auto* adult = spawnNear(ctx, feet, "minecraft:pig");
    auto* baby  = spawnNear(ctx, feet, "minecraft:pig<minecraft:entity_born>", -1);
    auto  aid = adult->getOrCreateUniqueID(), bid = baby->getOrCreateUniqueID();
    co_await ctx.ticks(2);
    ctx.require(baby->isBaby() && !adult->isBaby(), "a baby and an adult pig");
    carry::setKeyPressed(p, true);

    auto r = ep::tryPickup(p, *adult);
    ctx.check(r == Result::Lists, "a blacklisted adult is refused: " + want(r, Result::Lists));
    r = ep::tryPickup(p, *baby);
    ctx.check(r == Result::Lists, "a blacklisted baby is refused without allowBabies: " + want(r, Result::Lists));
    cfg.settings.allowBabies = true;
    r = ep::tryPickup(p, *adult);
    ctx.check(r == Result::Lists, "allowBabies does not help adults: " + want(r, Result::Lists));
    r = ep::tryPickup(p, *baby);
    ctx.check(r == Result::PickedUp, "allowBabies lets a blacklisted baby pass: " + want(r, Result::PickedUp));
    removeIfAlive(aid);
    removeIfAlive(bid);
    reset(p);
});

LO_REGISTER_SELFTEST("entitypickup.blacklist", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p     = ctx.player();
    auto  feet  = co_await arena(ctx, 330);
    auto* stand = spawnNear(ctx, feet, "minecraft:armor_stand");
    auto  id    = stand->getOrCreateUniqueID();
    co_await ctx.ticks(2);
    carry::setKeyPressed(p, true);
    auto r = ep::tryPickup(p, *stand);
    ctx.check(r == Result::Lists, "armor stands are blacklisted: " + want(r, Result::Lists));
    p.setPlayerGameType(GameType::Creative);
    r = ep::tryPickup(p, *stand);
    ctx.check(r == Result::Lists, "creative does not bypass the lists: " + want(r, Result::Lists));
    ctx.check(!carry::isCarrying(p), "nothing carried");
    removeIfAlive(id);
    reset(p);
});

LO_REGISTER_SELFTEST("entitypickup.hostile", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto&         p = ctx.player();
    ConfigRestore restore;
    auto          feet  = co_await arena(ctx, 336);
    auto          level = ll::service::getLevel();
    struct DifficultyRestore {
        Level&                           level;
        SharedTypes::Legacy::Difficulty saved;
        ~DifficultyRestore() { level.setDifficulty(saved); }
    } difficulty{*level, level->getDifficulty()}; // restored even when the test aborts
    level->setDifficulty(SharedTypes::Legacy::Difficulty::Easy); // monsters despawn in peaceful
    auto* zombie = spawnNear(ctx, feet, "minecraft:zombie");
    auto  id     = zombie->getOrCreateUniqueID();
    carry::setKeyPressed(p, true);
    auto r = ep::tryPickup(p, *zombie);
    ctx.check(r == Result::Hostile, "a zombie is refused in survival: " + want(r, Result::Hostile));
    lo::config::mut<carry::CarryConfig>().settings.pickupHostileMobs = true;
    r = ep::tryPickup(p, *zombie);
    ctx.check(r == Result::Size || r == Result::PickedUp, "pickupHostileMobs passes the hostile check: " + std::string(ep::resultName(r)));
    if (r == Result::PickedUp) {
        reset(p);
        co_await ctx.ticks(1);
        zombie = spawnNear(ctx, feet, "minecraft:zombie");
        id     = zombie->getOrCreateUniqueID();
        carry::setKeyPressed(p, true);
    }
    lo::config::mut<carry::CarryConfig>().settings.pickupHostileMobs = false;
    p.setPlayerGameType(GameType::Creative);
    r = ep::tryPickup(p, *zombie);
    ctx.check(r == Result::PickedUp, "creative picks up a zombie: " + want(r, Result::PickedUp));
    removeIfAlive(id);
    reset(p);
});

LO_REGISTER_SELFTEST("entitypickup.size", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p     = ctx.player();
    auto  feet  = co_await arena(ctx, 342);
    auto* golem = spawnNear(ctx, feet, "minecraft:iron_golem", 2);
    auto  id    = golem->getOrCreateUniqueID();
    co_await ctx.ticks(2);
    ctx.info("iron golem " + std::to_string(carry::entityHeight(*golem)) + " x " + std::to_string(carry::entityWidth(*golem)));
    carry::setKeyPressed(p, true);
    auto r = ep::tryPickup(p, *golem);
    ctx.check(r == Result::Size, "an iron golem (2.9 tall) is too big in survival: " + want(r, Result::Size));
    p.setPlayerGameType(GameType::Creative);
    r = ep::tryPickup(p, *golem);
    ctx.check(r == Result::PickedUp, "creative ignores the size: " + want(r, Result::PickedUp));
    removeIfAlive(id);
    reset(p);
});

LO_REGISTER_SELFTEST("entitypickup.owner", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p     = ctx.player();
    auto& other = ctx.spawnPlayer("lo_ep_owner");
    auto  feet  = co_await arena(ctx, 348);
    auto* wolf  = spawnNear(ctx, feet, "minecraft:wolf");
    auto* mine  = spawnNear(ctx, feet, "minecraft:wolf", -1);
    auto  wid = wolf->getOrCreateUniqueID(), mid = mine->getOrCreateUniqueID();
    auto  tame = [&](Actor& w, Player& owner) {
        auto c = w.getEntityContext().tryGetComponent<TameableComponent>();
        ctx.require(c.has_value(), "wolves are tameable");
        c->tame(w, owner);
    };
    tame(*wolf, other);
    tame(*mine, p);
    co_await ctx.ticks(2);
    ctx.require(wolf->isTame() && mine->isTame(), "both wolves are tamed");
    pin(*wolf, feet);
    pin(*mine, feet, -1);
    carry::setKeyPressed(p, true);
    auto r = ep::tryPickup(p, *wolf);
    ctx.check(r == Result::Owner, "another player's wolf is refused: " + want(r, Result::Owner));
    p.setPlayerGameType(GameType::Creative);
    r = ep::tryPickup(p, *wolf);
    ctx.check(r == Result::Owner, "creative does not bypass the owner rule: " + want(r, Result::Owner));
    p.setPlayerGameType(GameType::Survival);
    r = ep::tryPickup(p, *mine);
    ctx.check(r == Result::PickedUp, "the player's own wolf is picked up: " + want(r, Result::PickedUp));
    removeIfAlive(wid);
    removeIfAlive(mid);
    reset(p);
});

LO_REGISTER_SELFTEST("entitypickup.hurt", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p    = ctx.player();
    auto  feet = co_await arena(ctx, 354);
    auto* pig  = spawnNear(ctx, feet, "minecraft:pig");
    auto  id   = pig->getOrCreateUniqueID();
    co_await ctx.ticks(2);
    ctx.require(pig->hurtByCause(1.0f, SharedTypes::Legacy::ActorDamageCause::EntityAttack, p), "the pig is hurt");
    carry::setKeyPressed(p, true);
    auto r = ep::tryPickup(p, *pig);
    ctx.check(r == Result::Invulnerable, "a just-hurt pig is refused: " + want(r, Result::Invulnerable));
    co_await ctx.ticks(30);
    pig = ll::service::getLevel()->fetchEntity(id, false);
    ctx.require(pig != nullptr, "the pig is still there");
    pin(*pig, feet);
    r = ep::tryPickup(p, *pig);
    ctx.check(r == Result::PickedUp, "picked up once the hurt frames are over: " + want(r, Result::PickedUp));
    removeIfAlive(id);
    reset(p);
});

LO_REGISTER_SELFTEST("entitypickup.leash", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p    = ctx.player();
    auto  feet = co_await arena(ctx, 360);
    auto* pig  = spawnNear(ctx, feet, "minecraft:pig");
    co_await ctx.ticks(1);
    LeashableUtility::leash(*pig, p, false, false);
    co_await ctx.ticks(2);
    ctx.require(pig->isLeashed(), "the pig is on a lead");
    Vec3 at = pig->getPosition();
    carry::setKeyPressed(p, true);
    auto r = ep::tryPickup(p, *pig);
    ctx.require(r == Result::PickedUp, "leashed pig picked up: " + want(r, Result::PickedUp));
    co_await ctx.ticks(2);
    auto& reg   = p.getDimensionBlockSource();
    AABB  box{at - Vec3{3.0f, 3.0f, 3.0f}, at + Vec3{3.0f, 3.0f, 3.0f}};
    int   leads = 0;
    for (auto* e : reg.getEntities(box, 2.0f, ActorType::ItemEntity)) {
        auto& item = static_cast<ItemActor*>(e)->mItem;
        if (item->getTypeName() == "minecraft:lead") {
            ++leads;
            e->remove();
        }
    }
    ctx.check(leads == 1, "the lead dropped as an item, found " + std::to_string(leads));
    auto tag = carry::parseNbt(carry::get(p).entity);
    ctx.require(tag.has_value(), "entity NBT parses");
    bool leashed = tag->contains("LeasherID") && (*tag)["LeasherID"].get<Int64Tag>().data != -1;
    ctx.check(!leashed, "the carried pig is saved without its lead");
    reset(p);
});

LO_REGISTER_SELFTEST("entitypickup.riding", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p     = ctx.player();
    auto  feet  = co_await arena(ctx, 366);
    auto* horse = spawnNear(ctx, feet, "minecraft:pig");
    auto* rider = spawnNear(ctx, feet, "minecraft:chicken");
    auto  rid   = rider->getOrCreateUniqueID();
    ctx.require(rider->startRiding(*horse, true), "the chicken rides the pig");
    co_await ctx.ticks(2);
    carry::setKeyPressed(p, true);
    auto r = ep::tryPickup(p, *horse);
    ctx.require(r == Result::PickedUp, "the ridden pig is picked up: " + want(r, Result::PickedUp));
    co_await ctx.ticks(2);
    auto* c = ll::service::getLevel()->fetchEntity(rid, false);
    ctx.check(c != nullptr && c->getVehicle() == nullptr, "the passenger was ejected and stays in the world");
    removeIfAlive(rid);
    reset(p);
});

// The real interaction path: GameMode::interact through SimulatedPlayer::simulateInteract.
LO_REGISTER_SELFTEST("entitypickup.interact", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p    = ctx.player();
    auto& sim  = static_cast<SimulatedPlayer&>(p);
    auto  feet = co_await arena(ctx, 372);
    auto* cow  = spawnNear(ctx, feet, "minecraft:cow");
    auto  cid  = cow->getOrCreateUniqueID();
    co_await ctx.ticks(2);

    // Refused (an item in the hand): the vanilla interaction (milking) still happens.
    carry::setKeyPressed(p, true);
    int slot = p.getSelectedItemSlot();
    ctx.give(p, lo::game::makeItem("minecraft:bucket"), slot);
    pin(*cow, feet);
    sim.simulateLookAt(*cow);
    (void)ep::takeLastHookResult();
    bool used = sim.simulateInteract(*cow);
    ctx.info(std::string("simulateInteract with a bucket returned ") + (used ? "true" : "false"));
    co_await ctx.ticks(2);
    ctx.check(!carry::isCarrying(p), "no pickup with an item in the hand");
    bool milked = false;
    for (int i = 0; i < p.getInventory().getContainerSize(); ++i)
        if (p.getInventory().getItem(i).getTypeName() == "minecraft:milk_bucket") milked = true;
    ctx.check(milked, "the refused pickup keeps the vanilla interaction (cow milked)");
    for (int i = 0; i < p.getInventory().getContainerSize(); ++i) p.getInventory().setItem(i, ItemStack{});
    co_await ctx.ticks(1);

    // A cow has no vanilla interaction for an empty hand: it offers the carry interaction only while
    // the pickup is possible (that is what makes clients send the interaction at all).
    cow = ll::service::getLevel()->fetchEntity(cid, false);
    ctx.require(cow != nullptr, "the cow is still there");
    pin(*cow, feet);
    auto offered = [&] { return static_cast<bool>(*cow->getInteraction(p, cow->getPosition()).mCallback); };
    carry::setKeyPressed(p, false);
    ctx.check(!offered(), "no interaction offered without the carry key");
    carry::setKeyPressed(p, true);
    ctx.check(offered(), "the carry interaction is offered with the key and empty hands");

    // Accepted: the cow is picked up through the real path.
    sim.simulateLookAt(*cow);
    (void)ep::takeLastHookResult();
    used      = sim.simulateInteract(*cow);
    auto hook = ep::takeLastHookResult();
    ctx.check(used, "simulateInteract reports the interaction as used");
    ctx.check(hook == Result::PickedUp, std::string("the interact hook picked up, got ") + (hook ? std::string(ep::resultName(*hook)) : std::string("no call")));
    co_await ctx.ticks(2);
    ctx.check(carry::isCarrying(p) && carry::get(p).isEntity(), "simulateInteract picks the cow up");
    ctx.check(ll::service::getLevel()->fetchEntity(cid, false) == nullptr, "the cow is gone from the world");
    reset(p);

    // Players are left to U5: the interaction is not consumed here.
    auto& other = ctx.spawnPlayer("lo_ep_target");
    other.teleport(BlockPos{feet.x + 1, feet.y, feet.z}.bottomCenter(), other.getDimensionId());
    co_await ctx.ticks(2);
    carry::setKeyPressed(p, true);
    auto r = ep::tryPickup(p, other);
    ctx.check(r == Result::Ignored, "players are ignored by entity pickup: " + want(r, Result::Ignored));
    removeIfAlive(cid);
    reset(p);
});

// A pickup refused late (protection, step 10) keeps the vanilla interaction; an accepted one replaces it.
LO_REGISTER_SELFTEST("entitypickup.protection_keeps_vanilla", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p    = ctx.player();
    auto& sim  = static_cast<SimulatedPlayer&>(p);
    auto  feet = co_await arena(ctx, 378);
    auto* wolf = spawnNear(ctx, feet, "minecraft:wolf");
    auto  wid  = wolf->getOrCreateUniqueID();
    auto  c    = wolf->getEntityContext().tryGetComponent<TameableComponent>();
    ctx.require(c.has_value(), "wolves are tameable");
    c->tame(*wolf, p);
    co_await ctx.ticks(2);
    pin(*wolf, feet);
    bool sitting = wolf->getStatusFlag(ActorFlags::Sitting);
    carry::setKeyPressed(p, true);

    gRefuseEntities = true;
    sim.simulateLookAt(*wolf);
    (void)ep::takeLastHookResult();
    sim.simulateInteract(*wolf);
    auto hook       = ep::takeLastHookResult();
    gRefuseEntities = false;
    co_await ctx.ticks(2);
    ctx.check(hook == Result::Protection, std::string("refused by the protection hook, got ") + (hook ? std::string(ep::resultName(*hook)) : std::string("no call")));
    ctx.check(!carry::isCarrying(p), "nothing carried");
    wolf = ll::service::getLevel()->fetchEntity(wid, false);
    ctx.require(wolf != nullptr, "the wolf is still there");
    ctx.check(wolf->getStatusFlag(ActorFlags::Sitting) != sitting, "the vanilla interaction (sit) ran");

    pin(*wolf, feet);
    sim.simulateLookAt(*wolf);
    sim.simulateInteract(*wolf);
    hook = ep::takeLastHookResult();
    ctx.check(hook == Result::PickedUp, std::string("picked up without the veto, got ") + (hook ? std::string(ep::resultName(*hook)) : std::string("no call")));
    ctx.check(carry::isCarrying(p), "the wolf is carried");
    removeIfAlive(wid);
    reset(p);
});
