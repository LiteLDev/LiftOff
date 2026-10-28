#pragma once

// Configuration of the carry features (docs/SPEC.md §5). Keys and defaults are Carry On's, the
// default lists translated to Bedrock ids.
//
//   config/carryon.json         CarryConfig        (server and integrated server)
//   config/carryon_client.json  CarryClientConfig  (read by the client build; harmless on BDS)
//
//   auto const& s = lo::config::get<lo::feature::carry::CarryConfig>().settings;
//   if (s.pickupAllBlocks) ...
//
// Values are clamped to their ranges when loaded (and on /carryon reload). Lists are compiled by
// `feature/carry/Lists.h`; read the raw strings here only for formats a unit parses itself
// (customPickupConditions: U8, modelOverrides: U12).

#include <string>
#include <vector>

namespace lo::feature::carry {

struct CarryConfig {
    int version = 1;

    struct Settings {
        double maxDistance              = 2.5; ///< >= 0
        double maxEntityWidth           = 1.5; ///< 0..10
        double maxEntityHeight          = 2.5; ///< 0..10
        double blockSlownessMultiplier  = 1.0; ///< >= 0
        double entitySlownessMultiplier = 1.0; ///< >= 0
        int    maxEntityStackLimit      = 10;  ///< >= 1
        bool   heavyTiles               = true;
        bool   pickupAllBlocks          = false;
        bool   slownessInCreative       = true;
        bool   pickupHostileMobs        = false;
        bool   heavyEntities            = true;
        bool   allowBabies              = false;
        bool   useWhitelistBlocks       = false;
        bool   useWhitelistEntities     = false;
        bool   useWhitelistStacking     = false;
        bool   hitWhileCarrying         = false;
        bool   dropCarriedWhenHit       = false;
        bool   useScripts               = false;
        bool   stackableEntities        = true;
        bool   entitySizeMattersStacking = true;
        bool   pickupPlayers            = true;
        bool   pickupUnbreakableBlocks  = false;
        bool   scaleDistance            = true;
        bool   relativePlayerScale      = true;
        /// `id[state,...]`: states recomputed on placement for every block that has them.
        std::vector<std::string> placementStateExceptions = {
            "minecraft:stone_button[facing_direction]", "minecraft:grindstone[attachment]",
            "minecraft:vine[vine_direction_bits]", "minecraft:glow_lichen[multi_face_direction_bits]",
            "minecraft:standing_sign[ground_sign_direction]", "minecraft:trapdoor[upside_down_bit]"
        };
    } settings;

    struct Whitelist {
        std::vector<std::string> allowedEntities;
        std::vector<std::string> allowedBlocks;
        std::vector<std::string> allowedStacking;
    } whitelist;

    struct Blacklist {
        std::vector<std::string> forbiddenTiles = {
            "minecraft:end_portal", "minecraft:piston_arm_collision", "minecraft:sticky_piston_arm_collision",
            "minecraft:end_gateway", "minecraft:tall_grass", "minecraft:large_fern", "minecraft:peony",
            "minecraft:rose_bush", "minecraft:lilac", "minecraft:sunflower", "minecraft:bed", "minecraft:*_door",
            "minecraft:big_dripleaf[big_dripleaf_head=false]", "minecraft:waterlily", "minecraft:cake",
            "minecraft:portal", "minecraft:seagrass[sea_grass_type=double_top]",
            "minecraft:seagrass[sea_grass_type=double_bot]", "minecraft:frame", "minecraft:glow_frame",
            "animania:block_trough", "animania:block_invisiblock", "colossalchests:*", "ic2:*", "bigreactors:*",
            "forestry:*", "tconstruct:*", "rustic:*", "botania:*", "astralsorcery:*", "quark:colored_bed_*",
            "immersiveengineering:*", "embers:block_furnace", "embers:ember_bore", "embers:ember_activator",
            "embers:mixer", "embers:heat_coil", "embers:large_tank", "embers:crystal_cell",
            "embers:alchemy_pedestal", "embers:boiler", "embers:combustor", "embers:catalzyer", "embers:field_chart",
            "embers:inferno_forge", "storagedrawers:framingtable", "skyresources:*", "lootbags:*", "exsartagine:*",
            "aquamunda:tank", "opencomputers:*", "malisisdoors:*", "industrialforegoing:*", "minecolonies:*",
            "thaumcraft:pillar*", "thaumcraft:infernal_furnace", "thaumcraft:placeholder*",
            "thaumcraft:infusion_matrix", "thaumcraft:golem_builder", "thaumcraft:thaumatorium*",
            "magneticraft:oil_heater", "magneticraft:solar_panel", "magneticraft:steam_engine",
            "magneticraft:shelving_unit", "magneticraft:grinder", "magneticraft:sieve", "magneticraft:solar_tower",
            "magneticraft:solar_mirror", "magneticraft:container", "magneticraft:pumpjack", "magneticraft:refinery",
            "magneticraft:hydraulic_press", "magneticraft:multiblock_gap", "refinedstorage:*", "mcmultipart:*",
            "enderstorage:*", "betterstorage:*", "practicallogistics2:*", "wearablebackpacks:*", "rftools:screen",
            "rftools:creative_screen", "create:*", "magic_doorknob:*", "iceandfire:*", "ftbquests:*", "waystones:*",
            "contact:*", "framedblocks:*", "securitycraft:*", "forgemultipartcbe:*", "integrateddynamics:cable",
            "mekanismgenerators:wind_generator", "cookingforblockheads:cabinet", "cookingforblockheads:corner",
            "cookingforblockheads:counter", "cookingforblockheads:oven", "cookingforblockheads:toaster",
            "cookingforblockheads:milk_jar", "cookingforblockheads:cow_jar", "cookingforblockheads:fruit_basket",
            "cookingforblockheads:cooking_table", "cookingforblockheads:fridge", "cookingforblockheads:sink",
            "chipped:*", "irons_spellbooks:*", "create*:*", "simple_pipes:*", "libmultipart:*", "quark:tiny_potato",
            "ait:*", "vampirism:*", "extrastorage:*", "relics:researching_table", "sophisticatedstorage:*chest",
            "powah:*", "advancementtrophies:trophy", "mekanismgenerators:heat_generator", "mna:filler_block",
            "create_enchantment_industry:*", "graveyard:*", "immersivepetroleum:*", "tardis:interior_door",
            "cuffed:*", "littletiles:*", "butchersdelight:*", "extended_drawers:*", "functionalstorage:*",
            "sophisticatedstorage:*", "farmersdelight:*", "modern_industrialization:*_cable",
            "modern_industrialization:item_pipe", "modern_industrialization:*_item_pipe",
            "modern_industrialization:fluid_pipe", "modern_industrialization:*_fluid_pipe", "yigd:*",
            "domum_ornamentum:*", "connectiblechains:*", "cobblemon:*", "snowrealmagic:*",
            "forbidden_arcanus:black_hole", "sgjourney:*", "copycats:*", "expandedstorage:*", "replication:*",
            "coloreditemframes:*"
        };
        std::vector<std::string> forbiddenEntities = {
            "minecraft:ender_crystal", "minecraft:ender_dragon", "minecraft:ghast", "minecraft:fireball",
            "minecraft:small_fireball", "minecraft:wither_skull", "minecraft:shulker", "minecraft:leash_knot",
            "minecraft:armor_stand", "minecraft:wither_skull_dangerous", "minecraft:dragon_fireball",
            "minecraft:painting", "minecraft:shulker_bullet", "minecraft:evocation_fang", "minecraft:tnt",
            "minecraft:thrown_trident", "minecraft:arrow", "animania:hamster", "animania:ferret*",
            "animania:hedgehog*", "animania:cart", "animania:wagon", "mynko:*", "pixelmon:*", "mocreatures:*",
            "quark:totem", "vehicle:*", "securitycraft:*", "taterzens:npc", "easy_npc:*", "bodiesbodies:dead_body",
            "littletiles:*", "connectiblechains:*", "cobblemon:*", "create:*", "swem:*", "toms_mobs:*",
            "simulated:*", "aeronautics:*", "offroad:*", "coloreditemframes:*"
        };
        std::vector<std::string> forbiddenStacking = {
            "minecraft:horse", "minecraft:ender_dragon"
        };
    } blacklist;

    struct CustomPickupConditions {
        /// `id[state=value,...](stage)`
        std::vector<std::string> customPickupConditionsBlocks;
        /// `id(stage)`
        std::vector<std::string> customPickupConditionsEntities;
    } customPickupConditions;
};

struct CarryClientConfig {
    int version = 1;

    struct Settings {
        bool facePlayer             = false;
        bool rotateEntitiesSideways = false;
        bool renderArms             = true;
    } settings;

    struct ModelOverrides {
        /// `block[state=value,...]{snbt}->(item|block)target`
        std::vector<std::string> modelOverrides = {
            "minecraft:redstone_wire->(item)minecraft:redstone", "minecraft:bamboo_sapling->(block)minecraft:bamboo",
            "minecraft:candle_cake->(block)minecraft:cake"
        };
    } modelOverrides;
};

/// Clamps every value of `cfg` to its range (called after each load).
void clampConfig(CarryConfig& cfg);

} // namespace lo::feature::carry
