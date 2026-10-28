#include "feature/carry/CarryData.h"

namespace lo::feature::carry {

std::string_view carryTypeName(CarryType type) {
    switch (type) {
    case CarryType::Block:
        return "block";
    case CarryType::Entity:
        return "entity";
    case CarryType::Player:
        return "player";
    default:
        return "invalid";
    }
}

CarryType carryTypeFromName(std::string_view name) {
    if (name == "block") return CarryType::Block;
    if (name == "entity") return CarryType::Entity;
    if (name == "player") return CarryType::Player;
    return CarryType::Invalid;
}

void CarryData::clear() {
    type = CarryType::Invalid;
    block.clear();
    blockEntity.clear();
    entity.clear();
    player.clear();
    activeScript.clear();
    scriptRender = nullptr;
}

std::size_t CarryData::nbtSize() const {
    // Mirrors the fields Carry On writes: {type:"..",keyPressed:0b,block:{},tile:{},entity:{},
    // player:[I;a,b,c,d],activeScript:"..",selected:N,tick:N}
    std::string s = "{type:\"" + std::string(carryTypeName(type)) + "\",keyPressed:" + (keyPressed ? "1b" : "0b");
    if (!block.empty()) s += ",block:" + block;
    if (!blockEntity.empty()) s += ",tile:" + blockEntity;
    if (!entity.empty()) s += ",entity:" + entity;
    if (!player.empty()) s += ",player:\"" + player + "\"";
    if (!activeScript.empty()) s += ",activeScript:\"" + activeScript + "\"";
    s += ",selected:" + std::to_string(selected) + ",tick:" + std::to_string(tick) + "}";
    return s.size();
}

nlohmann::json CarryData::toJson() const {
    nlohmann::json j{
        {"type",     carryTypeName(type)},
        {"selected", selected           },
        {"tick",     tick               },
    };
    if (!block.empty()) j["block"] = block;
    if (!blockEntity.empty()) j["tile"] = blockEntity;
    if (!entity.empty()) j["entity"] = entity;
    if (!player.empty()) j["player"] = player;
    if (keyPressed) j["keyPressed"] = true;
    if (!activeScript.empty()) j["activeScript"] = activeScript;
    if (!scriptRender.is_null()) j["scriptRender"] = scriptRender;
    return j;
}

CarryData CarryData::fromJson(nlohmann::json const& j) {
    CarryData d;
    if (!j.is_object()) return d;
    auto str = [&](char const* key) {
        auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    d.type        = carryTypeFromName(str("type"));
    d.block       = str("block");
    d.blockEntity = str("tile");
    d.entity      = str("entity");
    d.player      = str("player");
    d.activeScript = str("activeScript");
    if (auto it = j.find("keyPressed"); it != j.end() && it->is_boolean()) d.keyPressed = it->get<bool>();
    if (auto it = j.find("scriptRender"); it != j.end()) d.scriptRender = *it;
    if (auto it = j.find("selected"); it != j.end() && it->is_number_integer()) d.selected = it->get<int>();
    if (auto it = j.find("tick"); it != j.end() && it->is_number_integer()) d.tick = it->get<std::int64_t>();
    return d;
}

} // namespace lo::feature::carry
