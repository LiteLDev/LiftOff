#include "feature/modeloverrides/ModelOverrides.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/ListTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/deps/nbt/Tag.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"

#include "feature/carry/Capture.h"
#include "feature/carry/CarryConfig.h"
#include "feature/carry/CarryData.h"
#include "feature/carry/Hooks.h"
#include "feature/carry/Lists.h"
#include "game/Blocks.h"
#include "game/Items.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lo::feature::modeloverrides {

namespace mo = core::modeloverride;
using carry::CarryData;
using carry::CarryVisual;

namespace {

// ---- compiled config ----------------------------------------------------------------------------

struct State {
    std::mutex                                           mutex;
    std::shared_ptr<std::vector<CompiledOverride> const> list = std::make_shared<std::vector<CompiledOverride>>();
    std::atomic<std::uint64_t>                           generation{1};
};

State& state() {
    static State s;
    return s;
}

std::shared_ptr<std::vector<CompiledOverride> const> currentList() {
    std::lock_guard lock(state().mutex);
    return state().list;
}

// ---- small per-thread caches (renderers ask every frame) ----------------------------------------

template <class V>
class Cache {
public:
    V const* find(std::string const& key) {
        sync();
        auto it = mMap.find(key);
        return it == mMap.end() ? nullptr : &it->second;
    }
    V const& put(std::string key, V value) {
        sync();
        if (mMap.size() >= 64) mMap.clear();
        return mMap.insert_or_assign(std::move(key), std::move(value)).first->second;
    }

private:
    void sync() {
        auto gen = state().generation.load(std::memory_order_acquire);
        if (gen != mGeneration) {
            mMap.clear();
            mGeneration = gen;
        }
    }
    std::unordered_map<std::string, V> mMap;
    std::uint64_t                      mGeneration = 0;
};

std::string cacheKey(std::initializer_list<std::string_view> parts) {
    std::string key;
    for (auto p : parts) {
        key += p;
        key += '\x1f';
    }
    return key;
}

std::optional<bool> asBool(std::string_view v) {
    if (v == "true" || v == "1" || v == "TRUE" || v == "True") return true;
    if (v == "false" || v == "0" || v == "FALSE" || v == "False") return false;
    return std::nullopt;
}

std::optional<int> asInt(std::string_view v) {
    int  out = 0;
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    if (ec != std::errc{} || p != v.data() + v.size()) return std::nullopt;
    return out;
}

void deepMerge(CompoundTag& dst, CompoundTag const& src) {
    for (auto const& [key, value] : src.mTags) {
        if (value.hold(Tag::Type::Compound)) {
            auto it = dst.mTags.find(key);
            if (it != dst.mTags.end() && it->second.hold(Tag::Type::Compound)) {
                deepMerge(it->second.get<CompoundTag>(), value.get<CompoundTag>());
                continue;
            }
        }
        dst.mTags.insert_or_assign(key, value);
    }
}

bool itemExists(std::string const& id) { return !game::makeItem(id).isNull(); }

} // namespace

// ---- parsing ------------------------------------------------------------------------------------

std::vector<CompiledOverride> compile(std::vector<std::string> const& entries, std::vector<std::string>* invalid) {
    std::vector<CompiledOverride> out;
    for (auto const& text : entries) {
        auto parsed = mo::parse(text);
        if (!parsed) {
            if (invalid && text.find_first_not_of(" \t\r\n") != std::string::npos) invalid->push_back(text);
            continue;
        }
        CompiledOverride c{std::move(*parsed), std::nullopt};
        if (!c.entry.nbt.empty()) {
            auto tag = carry::parseNbt(c.entry.nbt);
            if (!tag) {
                if (invalid) invalid->push_back(text);
                continue;
            }
            if (!tag->empty()) c.nbt = std::move(*tag);
        }
        out.push_back(std::move(c));
    }
    return out;
}

bool nbtPartialMatch(Tag const& expected, Tag const& actual) {
    if (expected.getId() != actual.getId()) return false;
    switch (expected.getId()) {
    case Tag::Type::Compound: {
        auto const& e = static_cast<CompoundTag const&>(expected);
        auto const& a = static_cast<CompoundTag const&>(actual);
        for (auto const& [key, value] : e.mTags) {
            auto it = a.mTags.find(key);
            if (it == a.mTags.end() || !nbtPartialMatch(value.get(), it->second.get())) return false;
        }
        return true;
    }
    case Tag::Type::List: {
        auto const& e = static_cast<ListTag const&>(expected);
        auto const& a = static_cast<ListTag const&>(actual);
        if (e.empty()) return a.empty();
        for (auto const& item : e) {
            if (!item.get()) continue;
            bool found = std::any_of(a.begin(), a.end(), [&](auto const& other) {
                return other.get() && nbtPartialMatch(*item.get(), *other.get());
            });
            if (!found) return false;
        }
        return true;
    }
    default:
        return expected.equals(actual);
    }
}

Block const* resolveBlock(std::string_view id, std::vector<core::carry::StateFilter> const& states) {
    auto const* block = game::blockWith(id);
    if (!block || states.empty()) return block;
    CompoundTag tag   = *block->mSerializationId;
    auto*       found = tag.get("states");
    if (!found || found->getId() != Tag::Type::Compound) return nullptr;
    auto& compound = tag.at("states").get<CompoundTag>();
    for (auto const& f : states) {
        if (!f.value) return nullptr;
        auto const* current = compound.get(f.name);
        if (!current) return nullptr;
        switch (current->getId()) {
        case Tag::Type::Byte: {
            auto b = asBool(*f.value);
            if (!b) return nullptr;
            compound.at(f.name) = ByteTag{*b ? 1 : 0};
            break;
        }
        case Tag::Type::Int: {
            auto i = asInt(*f.value);
            if (!i) return nullptr;
            compound.at(f.name) = IntTag{*i};
            break;
        }
        case Tag::Type::String:
            compound.at(f.name) = StringTag{*f.value};
            break;
        default:
            return nullptr;
        }
    }
    auto resolved = Block::tryGetFromRegistry(tag);
    return resolved ? resolved.as_ptr() : nullptr;
}

std::string resolveBlockSnbt(std::string_view spec) {
    thread_local Cache<std::string> cache;
    std::string                     key(spec);
    if (auto const* hit = cache.find(key)) return *hit;
    std::string out;
    if (auto parsed = mo::parseBlockSpec(spec)) {
        if (auto const* block = resolveBlock(parsed->id, parsed->states)) out = carry::toSnbt(*block->mSerializationId);
    }
    return cache.put(std::move(key), std::move(out));
}

std::optional<OverrideResult>
findOverride(std::vector<CompiledOverride> const& list, Block const& block, CompoundTag const* blockEntity) {
    auto candidate = carry::blockCandidate(block);
    for (std::size_t i = 0; i < list.size(); ++i) {
        auto const& c = list[i];
        if (!c.entry.source.matches(candidate)) continue;
        if (c.nbt && (!blockEntity || !nbtPartialMatch(*c.nbt, *blockEntity))) continue;
        OverrideResult r;
        r.index  = i;
        r.target = c.entry.targetText();
        if (c.entry.kind == mo::TargetKind::Item) {
            if (!itemExists(c.entry.target)) continue; // unknown target: the entry is ignored
            r.kind = CarryVisual::Kind::Item;
            r.item = c.entry.target;
        } else {
            auto const* target = resolveBlock(c.entry.target, c.entry.targetStates);
            if (!target) continue;
            r.kind  = CarryVisual::Kind::Block;
            r.block = carry::toSnbt(*target->mSerializationId);
        }
        return r;
    }
    return std::nullopt;
}

bool modelOverridesActive() {
#ifdef LO_CLIENT
    return true;
#else
    return false;
#endif
}

std::optional<OverrideResult> configuredOverride(std::string const& blockSnbt, std::string const& blockEntitySnbt) {
    thread_local Cache<std::optional<OverrideResult>> cache;
    auto                                              key = cacheKey({blockSnbt, blockEntitySnbt});
    if (auto const* hit = cache.find(key)) return *hit;
    std::optional<OverrideResult> result;
    auto                          list = currentList();
    if (!list->empty()) {
        if (auto tag = carry::parseNbt(blockSnbt)) {
            if (auto block = Block::tryGetFromRegistry(*tag)) {
                auto be = carry::parseNbt(blockEntitySnbt);
                result  = findOverride(*list, *block.as_ptr(), be ? &*be : nullptr);
            }
        }
    }
    return cache.put(std::move(key), std::move(result));
}

std::string renderEntitySnbt(
    std::string const&                entitySnbt,
    std::optional<std::string> const& nameEntity,
    std::optional<std::string> const& nbt
) {
    if (!nameEntity && !nbt) return entitySnbt;
    thread_local Cache<std::string> cache;
    auto key = cacheKey({entitySnbt, nameEntity.value_or(""), nameEntity ? "1" : "0", nbt.value_or("")});
    if (auto const* hit = cache.find(key)) return *hit;
    CompoundTag tag;
    if (nameEntity) {
        // A fresh entity of that type (Carry On creates a new entity for `name_entity`).
        tag.putString("identifier", *nameEntity);
    } else if (auto parsed = carry::parseNbt(entitySnbt)) {
        tag = std::move(*parsed);
    } else {
        return cache.put(std::move(key), entitySnbt);
    }
    if (nbt) {
        if (auto extra = carry::parseNbt(*nbt)) deepMerge(tag, *extra);
    }
    return cache.put(std::move(key), carry::toSnbt(tag));
}

void applyVisual(CarryData const& data, CarryVisual& v, bool useModelOverrides) {
    std::optional<mo::ScriptRender> render;
    if (data.scriptRender.is_object() && !data.scriptRender.empty()) render = mo::parseScriptRender(data.scriptRender);

    if (v.kind == CarryVisual::Kind::Block) {
        bool scripted = false;
        if (render && render->nameBlock) {
            if (auto snbt = resolveBlockSnbt(*render->nameBlock); !snbt.empty()) {
                v.block  = std::move(snbt);
                scripted = true; // script render wins over model overrides (SPEC §4.4)
            }
        }
        if (!scripted && useModelOverrides) {
            if (auto r = configuredOverride(v.block, data.blockEntity)) {
                v.kind = r->kind;
                if (r->kind == CarryVisual::Kind::Item) {
                    v.item         = r->item;
                    v.itemOverride = true;
                } else {
                    v.block = r->block;
                }
            }
        }
    } else if (v.kind == CarryVisual::Kind::Entity && render) {
        v.entity = renderEntitySnbt(v.entity, render->nameEntity, render->nbt);
    }

    if (render) {
        v.translation = render->translation;
        v.rotation    = render->rotation;
        v.scale       = render->scale;
        for (int i = 0; i < 3; ++i) {
            if (render->leftArm[i]) v.leftArm[i] = render->leftArm[i];
            if (render->rightArm[i]) v.rightArm[i] = render->rightArm[i];
        }
        v.renderLeftArm  = render->renderLeftArm;
        v.renderRightArm = render->renderRightArm;
    }
}

void rebuild() {
    std::vector<std::string> invalid;
    auto list = std::make_shared<std::vector<CompiledOverride> const>(
        compile(config::get<carry::CarryClientConfig>().modelOverrides.modelOverrides, &invalid)
    );
    for (auto const& e : invalid) log().warn("carryon_client.json modelOverrides: invalid entry '{}' ignored", e);
    {
        std::lock_guard lock(state().mutex);
        state().list = std::move(list);
    }
    state().generation.fetch_add(1, std::memory_order_acq_rel);
}

namespace {
class ModelOverridesModule {
public:
    void install() {
        rebuild();
        if (!mHooked) {
            config::onReload([] { rebuild(); });
            mHooked = true;
        }
    }
    void uninstall() {}

private:
    bool mHooked = false;
};
} // namespace

} // namespace lo::feature::modeloverrides

LO_REGISTER_MODULE(lo::feature::modeloverrides::ModelOverridesModule);

LO_REGISTER_VISUAL_PROVIDER(lo::feature::carry::VisualProvider{
    .name     = "modeloverrides",
    .priority = 0,
    .apply =
        [](lo::feature::carry::CarryData const& data, lo::feature::carry::CarryVisual& visual) {
            namespace m = lo::feature::modeloverrides;
            m::applyVisual(data, visual, m::modelOverridesActive());
        },
});

// `/carryon debug`: "Override Model: <target>" when a model override matches the carried block
// (client build only: the integrated server shares the client config, a dedicated server has none).
LO_REGISTER_DEBUG_LINES([](Player const& player, lo::feature::carry::CarryData const& data, std::vector<std::string>& lines) {
    namespace m = lo::feature::modeloverrides;
    if (!data.isBlock() || !m::modelOverridesActive()) return;
    if (auto r = m::configuredOverride(data.block, data.blockEntity)) {
        lines.push_back(lo::trFor(player, "lo.carry.debug.override", "Override Model: {}", r->target));
    }
});
