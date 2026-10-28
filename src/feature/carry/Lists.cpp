#include "feature/carry/Lists.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/level/block/Block.h"

#include "feature/carry/CarryConfig.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::feature::carry {

using core::carry::Candidate;
using core::carry::IdList;
using core::carry::StateExceptions;

namespace {
struct Compiled {
    IdList          forbiddenTiles, forbiddenEntities, forbiddenStacking;
    IdList          allowedBlocks, allowedEntities, allowedStacking;
    StateExceptions exceptions;
};

Compiled& compiled() {
    static Compiled c;
    return c;
}

IdList compile(std::vector<std::string> const& entries, char const* name) {
    std::vector<std::string> invalid;
    IdList                   list(entries, &invalid);
    for (auto const& e : invalid) log().warn("carryon.json {}: invalid entry '{}' ignored", name, e);
    return list;
}
} // namespace

std::optional<std::string> blockStateValue(Block const& block, std::string_view name) {
    auto const* states = block.mSerializationId->get("states");
    if (!states || states->getId() != Tag::Type::Compound) return std::nullopt;
    auto const* tag = static_cast<CompoundTag const*>(states)->get(name);
    if (!tag) return std::nullopt;
    switch (tag->getId()) {
    case Tag::Type::Byte:
        return static_cast<ByteTag const*>(tag)->data != 0 ? "true" : "false";
    case Tag::Type::Int:
        return std::to_string(static_cast<IntTag const*>(tag)->data);
    case Tag::Type::String:
        return std::string(static_cast<std::string const&>(*static_cast<StringTag const*>(tag)));
    default:
        return std::nullopt;
    }
}

Candidate blockCandidate(Block const& block) {
    return Candidate{
        block.getTypeName(),
        [&block](std::string_view tag) {
            for (auto const& t : block.mTags.get()) {
                if (t.getString() == tag) return true;
            }
            return false;
        },
        [&block](std::string_view name) { return blockStateValue(block, name); },
    };
}

Candidate entityCandidate(Actor const& actor) {
    return Candidate{
        actor.getTypeName(),
        [&actor](std::string_view family) { return actor.hasFamily(HashedString(std::string(family))); },
        {},
    };
}

bool isBlockAllowed(Block const& block) {
    auto const& c = compiled();
    return core::carry::isAllowed(
        c.forbiddenTiles,
        c.allowedBlocks,
        config::get<CarryConfig>().settings.useWhitelistBlocks,
        blockCandidate(block)
    );
}

bool isEntityAllowed(Actor const& actor) {
    auto const& c = compiled();
    return core::carry::isAllowed(
        c.forbiddenEntities,
        c.allowedEntities,
        config::get<CarryConfig>().settings.useWhitelistEntities,
        entityCandidate(actor)
    );
}

bool isStackingAllowed(Actor const& actor) {
    auto const& c = compiled();
    return core::carry::isAllowed(
        c.forbiddenStacking,
        c.allowedStacking,
        config::get<CarryConfig>().settings.useWhitelistStacking,
        entityCandidate(actor)
    );
}

bool isBlockIdAllowed(std::string_view id) {
    auto const& c = compiled();
    return core::carry::isAllowed(
        c.forbiddenTiles,
        c.allowedBlocks,
        config::get<CarryConfig>().settings.useWhitelistBlocks,
        Candidate{id, {}, {}}
    );
}

bool isEntityIdAllowed(std::string_view id) {
    auto const& c = compiled();
    return core::carry::isAllowed(
        c.forbiddenEntities,
        c.allowedEntities,
        config::get<CarryConfig>().settings.useWhitelistEntities,
        Candidate{id, {}, {}}
    );
}

bool isPlacementException(std::string_view stateName) { return compiled().exceptions.contains(stateName); }

void rebuildLists() {
    auto& cfg = config::mut<CarryConfig>();
    clampConfig(cfg);
    Compiled c;
    c.forbiddenTiles    = compile(cfg.blacklist.forbiddenTiles, "forbiddenTiles");
    c.forbiddenEntities = compile(cfg.blacklist.forbiddenEntities, "forbiddenEntities");
    c.forbiddenStacking = compile(cfg.blacklist.forbiddenStacking, "forbiddenStacking");
    c.allowedBlocks     = compile(cfg.whitelist.allowedBlocks, "allowedBlocks");
    c.allowedEntities   = compile(cfg.whitelist.allowedEntities, "allowedEntities");
    c.allowedStacking   = compile(cfg.whitelist.allowedStacking, "allowedStacking");
    std::vector<std::string> invalid;
    c.exceptions = StateExceptions(cfg.settings.placementStateExceptions, &invalid);
    for (auto const& e : invalid) log().warn("carryon.json placementStateExceptions: invalid entry '{}' ignored", e);
    compiled() = std::move(c);
}

class CarryListsModule {
public:
    void install() {
        rebuildLists();
        if (!mHooked) {
            config::onReload([] { rebuildLists(); });
            mHooked = true;
        }
    }
    void uninstall() {}

private:
    bool mHooked = false;
};

} // namespace lo::feature::carry

LO_REGISTER_MODULE_ORDERED(lo::feature::carry::CarryListsModule, lo::ModuleOrder::Services);
