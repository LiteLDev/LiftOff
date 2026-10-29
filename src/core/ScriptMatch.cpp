#include "core/ScriptMatch.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <utility>

namespace lo::core::scripts {

using nlohmann::json;

namespace {
std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// The whole (trimmed) text as a double.
std::optional<double> toDouble(std::string_view s) {
    s = trim(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return std::nullopt;
    double v{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

std::optional<int> toInt(std::string_view s) {
    s = trim(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return std::nullopt;
    int v{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

/// Splits `text` on `sep`, trimming each part and skipping empty ones.
std::vector<std::string_view> split(std::string_view text, char sep) {
    std::vector<std::string_view> out;
    while (true) {
        auto at   = text.find(sep);
        auto part = trim(text.substr(0, at));
        if (!part.empty()) out.push_back(part);
        if (at == std::string_view::npos) break;
        text.remove_prefix(at + 1);
    }
    return out;
}

/// `k=v,k=v` pairs (keys lowercased); nullopt if a part has no '=' or a value is not a number.
std::optional<std::vector<std::pair<std::string, double>>> parsePairs(std::string_view text) {
    std::vector<std::pair<std::string, double>> out;
    for (auto part : split(text, ',')) {
        auto eq = part.find('=');
        if (eq == std::string_view::npos) return std::nullopt;
        auto v = toDouble(part.substr(eq + 1));
        if (!v) return std::nullopt;
        out.emplace_back(lower(trim(part.substr(0, eq))), *v);
    }
    return out;
}
} // namespace

// ---- number bounds ---------------------------------------------------------------------------

bool NumberBound::test(double v) const {
    switch (op) {
    case Op::Le:
        return v <= value;
    case Op::Ge:
        return v >= value;
    case Op::Lt:
        return v < value;
    case Op::Gt:
        return v > value;
    case Op::Eq:
        return v == value;
    }
    return false;
}

std::optional<NumberBound> parseBound(std::string_view text) {
    text = trim(text);
    NumberBound b;
    if (text.starts_with("<=")) {
        b.op = NumberBound::Op::Le;
        text.remove_prefix(2);
    } else if (text.starts_with(">=")) {
        b.op = NumberBound::Op::Ge;
        text.remove_prefix(2);
    } else if (text.starts_with("<")) {
        b.op = NumberBound::Op::Lt;
        text.remove_prefix(1);
    } else if (text.starts_with(">")) {
        b.op = NumberBound::Op::Gt;
        text.remove_prefix(1);
    } else if (text.starts_with("=")) {
        b.op = NumberBound::Op::Eq;
        text.remove_prefix(1);
    }
    auto v = toDouble(text);
    if (!v) return std::nullopt;
    b.value = *v;
    return b;
}

bool matchesBound(std::string_view text, double value) {
    if (trim(text).empty()) return true;
    auto b = parseBound(text);
    return b && b->test(value);
}

// ---- conditions ------------------------------------------------------------------------------

std::optional<ScoreCondition> parseScore(std::string_view text) {
    text    = trim(text);
    auto op = text.find_first_of("<>=");
    if (op == std::string_view::npos || op == 0) return std::nullopt;
    auto bound = parseBound(text.substr(op));
    if (!bound) return std::nullopt;
    auto name = trim(text.substr(0, op));
    if (name.empty()) return std::nullopt;
    return ScoreCondition{std::string(name), *bound};
}

std::optional<PositionCondition> parsePosition(std::string_view text) {
    auto pairs = parsePairs(text);
    if (!pairs) return std::nullopt;
    PositionCondition c;
    for (auto const& [k, v] : *pairs) {
        if (k == "x") c.x = v;
        else if (k == "y") c.y = v;
        else if (k == "z") c.z = v;
        else if (k == "dx") c.dx = v;
        else if (k == "dy") c.dy = v;
        else if (k == "dz") c.dz = v;
        else return std::nullopt;
    }
    return c;
}

bool positionMatches(PositionCondition const& c, double x, double y, double z) {
    auto axis = [](double base, double d, double v) { return base == 0.0 || (v >= base && v <= base + d); };
    return axis(c.x, c.dx, x) && axis(c.y, c.dy, y) && axis(c.z, c.dz, z);
}

std::optional<std::array<double, 3>> parseXyz(std::string_view text, double def) {
    auto pairs = parsePairs(text);
    if (!pairs) return std::nullopt;
    std::array<double, 3> out{def, def, def};
    for (auto const& [k, v] : *pairs) {
        if (k == "x") out[0] = v;
        else if (k == "y") out[1] = v;
        else if (k == "z") out[2] = v;
        else return std::nullopt;
    }
    return out;
}

int effectId(std::string_view name) {
    auto n = lower(trim(name));
    if (n.starts_with("minecraft:")) n.erase(0, 10);
    static constexpr std::pair<std::string_view, int> kIds[] = {
        {"speed",                  1 },
        {"slowness",               2 },
        {"haste",                  3 },
        {"mining_fatigue",         4 },
        {"strength",               5 },
        {"instant_health",         6 },
        {"instant_damage",         7 },
        {"jump_boost",             8 },
        {"nausea",                 9 },
        {"regeneration",           10},
        {"resistance",             11},
        {"fire_resistance",        12},
        {"water_breathing",        13},
        {"invisibility",           14},
        {"blindness",              15},
        {"night_vision",           16},
        {"hunger",                 17},
        {"weakness",               18},
        {"poison",                 19},
        {"wither",                 20},
        {"health_boost",           21},
        {"absorption",             22},
        {"saturation",             23},
        {"levitation",             24},
        {"fatal_poison",           25},
        {"conduit_power",          26},
        {"slow_falling",           27},
        {"bad_omen",               28},
        {"village_hero",           29},
        {"hero_of_the_village",    29},
        {"darkness",               30},
        {"trial_omen",             31},
        {"wind_charged",           32},
        {"weaving",                33},
        {"oozing",                 34},
        {"infested",               35},
        {"raid_omen",              36},
        {"breath_of_the_nautilus", 37},
    };
    for (auto const& [k, id] : kIds) {
        if (k == n) return id;
    }
    return -1;
}

std::optional<std::vector<EffectRequirement>> parseEffects(std::string_view text) {
    std::vector<EffectRequirement> out;
    for (auto part : split(text, ',')) {
        EffectRequirement r;
        auto              hash = part.find('#');
        auto              name = trim(part.substr(0, hash));
        if (hash != std::string_view::npos) {
            auto amp = toInt(part.substr(hash + 1));
            if (!amp) return std::nullopt;
            r.amplifier = *amp;
        }
        r.name = lower(name);
        if (r.name.starts_with("minecraft:")) r.name.erase(0, 10);
        r.id = effectId(r.name);
        out.push_back(std::move(r));
    }
    return out;
}

int javaGameModeId(int bedrockGameType) {
    switch (bedrockGameType) {
    case 0:
        return 0; // survival
    case 1:
        return 1; // creative
    case 2:
        return 2; // adventure
    case 6:
        return 3; // spectator
    default:
        return -1;
    }
}

// ---- SNBT ------------------------------------------------------------------------------------

namespace {
class SnbtParser {
public:
    explicit SnbtParser(std::string_view s) : mS(s) {}

    std::optional<json> parseDocument() {
        auto v = value();
        if (!v) return std::nullopt;
        skipWs();
        if (mI != mS.size()) return std::nullopt;
        return v;
    }

private:
    std::string_view mS;
    std::size_t      mI = 0;
    int              mDepth = 0;

    /// Whitespace and `/* ... */` comments (Bedrock's SNBT writes `"..." /*BASE64*/` after base64 strings).
    void skipWs() {
        for (;;) {
            while (mI < mS.size() && std::isspace(static_cast<unsigned char>(mS[mI]))) ++mI;
            if (mS.substr(mI, 2) != "/*") return;
            auto end = mS.find("*/", mI + 2);
            mI       = end == std::string_view::npos ? mS.size() : end + 2;
        }
    }
    bool eat(char c) {
        skipWs();
        if (mI < mS.size() && mS[mI] == c) {
            ++mI;
            return true;
        }
        return false;
    }
    [[nodiscard]] char peek() {
        skipWs();
        return mI < mS.size() ? mS[mI] : '\0';
    }

    /// Characters of an unquoted key (values may also contain ':').
    static bool isBare(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '+';
    }

    std::optional<std::string> quoted() {
        char q = mS[mI++];
        std::string out;
        while (mI < mS.size()) {
            char c = mS[mI++];
            if (c == q) return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (mI >= mS.size()) return std::nullopt;
            char e = mS[mI++];
            switch (e) {
            case 'n':
                out.push_back('\n');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'u': {
                if (mI + 4 > mS.size()) return std::nullopt;
                unsigned cp = 0;
                auto [p, ec] = std::from_chars(mS.data() + mI, mS.data() + mI + 4, cp, 16);
                if (ec != std::errc{} || p != mS.data() + mI + 4) return std::nullopt;
                mI += 4;
                // UTF-8 encode (BMP only; surrogates are kept as is).
                if (cp < 0x80) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default:
                out.push_back(e); // \" \' \\ \/ and anything else literally
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> key() {
        skipWs();
        if (mI >= mS.size()) return std::nullopt;
        if (mS[mI] == '"' || mS[mI] == '\'') return quoted();
        auto start = mI;
        while (mI < mS.size() && isBare(mS[mI])) ++mI;
        if (mI == start) return std::nullopt;
        return std::string(mS.substr(start, mI - start));
    }

    /// A bare token: a number with an optional type suffix, true/false, or an unquoted string.
    static json bare(std::string_view tok) {
        auto l = lower(tok);
        if (l == "true") return 1;
        if (l == "false") return 0;
        std::string_view num = tok;
        if (!num.empty()) {
            char last = static_cast<char>(std::tolower(static_cast<unsigned char>(num.back())));
            if (last == 'b' || last == 's' || last == 'l' || last == 'f' || last == 'd') num.remove_suffix(1);
        }
        bool integral = !num.empty() && num.find_first_of(".eE") == std::string_view::npos;
        if (integral) {
            std::string_view n = num;
            if (n.front() == '+') n.remove_prefix(1);
            std::int64_t i{};
            auto [p, ec] = std::from_chars(n.data(), n.data() + n.size(), i);
            if (ec == std::errc{} && p == n.data() + n.size()) return i;
        }
        if (auto d = toDouble(num); d && num.find_first_not_of("0123456789+-.eE") == std::string_view::npos) {
            return *d;
        }
        return std::string(tok);
    }

    std::optional<json> value() {
        if (++mDepth > 512) return std::nullopt;
        struct Leave {
            int& d;
            ~Leave() { --d; }
        } leave{mDepth};
        char c = peek();
        if (c == '{') {
            ++mI;
            json obj = json::object();
            if (eat('}')) return obj;
            while (true) {
                auto k = key();
                if (!k || !eat(':')) return std::nullopt;
                auto v = value();
                if (!v) return std::nullopt;
                obj[*k] = std::move(*v);
                if (eat(',')) continue;
                if (eat('}')) return obj;
                return std::nullopt;
            }
        }
        if (c == '[') {
            ++mI;
            json arr = json::array();
            // typed arrays [B;..] [I;..] [L;..]
            skipWs();
            if (mI + 1 < mS.size() && (mS[mI] == 'B' || mS[mI] == 'I' || mS[mI] == 'L')) {
                auto save = mI;
                ++mI;
                if (eat(';')) {
                    // elements follow as usual
                } else {
                    mI = save;
                }
            }
            if (eat(']')) return arr;
            while (true) {
                auto v = value();
                if (!v) return std::nullopt;
                arr.push_back(std::move(*v));
                if (eat(',')) continue;
                if (eat(']')) return arr;
                return std::nullopt;
            }
        }
        if (c == '"' || c == '\'') {
            auto s = quoted();
            if (!s) return std::nullopt;
            return json(std::move(*s));
        }
        auto start = mI;
        while (mI < mS.size() && (isBare(mS[mI]) || mS[mI] == ':')) ++mI;
        if (mI == start) return std::nullopt;
        return bare(mS.substr(start, mI - start));
    }
};

std::optional<double> numberOf(json const& j) {
    if (j.is_boolean()) return j.get<bool>() ? 1.0 : 0.0;
    if (j.is_number()) return j.get<double>();
    return std::nullopt;
}
} // namespace

std::optional<json> parseSnbt(std::string_view snbt) {
    if (trim(snbt).empty()) return std::nullopt;
    return SnbtParser(snbt).parseDocument();
}

bool nbtMatches(json const& expected, json const& actual) {
    if (expected.is_null()) return true;
    if (expected.is_object()) {
        if (!actual.is_object()) return false;
        for (auto const& [k, v] : expected.items()) {
            auto it = actual.find(k);
            if (it == actual.end() || !nbtMatches(v, *it)) return false;
        }
        return true;
    }
    if (expected.is_array()) {
        if (!actual.is_array()) return false;
        if (expected.empty()) return actual.empty();
        for (auto const& e : expected) {
            if (std::none_of(actual.begin(), actual.end(), [&](json const& a) { return nbtMatches(e, a); })) return false;
        }
        return true;
    }
    if (expected.is_string()) return actual.is_string() && actual.get<std::string>() == expected.get<std::string>();
    auto a = numberOf(expected), b = numberOf(actual);
    return a && b && *a == *b;
}

// ---- scripts ---------------------------------------------------------------------------------

bool Script::hasConditions() const {
    return !gamestage.empty() || !advancement.empty() || !xp.empty() || !gamemode.empty() || !scoreboard.empty()
        || !position.empty() || !effects.empty();
}

namespace {
/// A string field (numbers are accepted and written as text). False if the type is wrong.
bool readString(json const& obj, char const* key, std::string& out, std::string* error, std::string_view where) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return true;
    if (it->is_string()) {
        out = it->get<std::string>();
    } else if (it->is_number_integer()) {
        out = std::to_string(it->get<std::int64_t>());
    } else if (it->is_number()) {
        out = it->dump();
    } else {
        if (error) *error = std::string(where) + "." + key + " must be a string";
        return false;
    }
    return true;
}
} // namespace

std::optional<Script> parseScript(json const& j, std::string id, std::string* error) {
    auto fail = [&](std::string msg) -> std::optional<Script> {
        if (error) *error = std::move(msg);
        return std::nullopt;
    };
    if (!j.is_object()) return fail("a script must be a JSON object");
    Script s;
    s.id = std::move(id);

    if (auto it = j.find("priority"); it != j.end() && !it->is_null()) {
        if (!it->is_number_integer()) return fail("priority must be an integer");
        s.priority = it->get<std::int64_t>();
    }
    if (auto it = j.find("override_checks"); it != j.end() && !it->is_null()) {
        if (!it->is_boolean()) return fail("override_checks must be true or false");
        s.overrideChecks = it->get<bool>();
    }

    auto obj = j.find("object");
    if (obj == j.end() || !obj->is_object()) return fail("missing object");
    bool hasBlock  = obj->contains("block");
    bool hasEntity = obj->contains("entity");
    if (hasBlock == hasEntity) return fail("object must have exactly one of block / entity");
    s.isBlock        = hasBlock;
    auto const& what = (*obj)[hasBlock ? "block" : "entity"];
    std::string where = hasBlock ? "object.block" : "object.entity";
    if (!what.is_object()) return fail(where + " must be an object");

    std::string name;
    if (!readString(what, "name", name, error, where)) return std::nullopt;
    if (!trim(name).empty()) {
        s.name = carry::parseEntry(name);
        if (!s.name) return fail(where + ".name is not a valid id: " + name);
    }
    if (hasBlock) {
        if (!readString(what, "hardness", s.hardness, error, where)) return std::nullopt;
        if (!readString(what, "resistance", s.resistance, error, where)) return std::nullopt;
    } else {
        if (!readString(what, "health", s.health, error, where)) return std::nullopt;
        if (!readString(what, "height", s.height, error, where)) return std::nullopt;
        if (!readString(what, "width", s.width, error, where)) return std::nullopt;
    }
    if (auto it = what.find("nbt"); it != what.end() && !it->is_null()) {
        if (!it->is_object()) return fail(where + ".nbt must be an object");
        s.nbt = *it;
    }

    bool hasConditions = false, hasRender = false, hasEffects = false;
    if (auto it = j.find("conditions"); it != j.end() && !it->is_null()) {
        if (!it->is_object()) return fail("conditions must be an object");
        hasConditions = true;
        auto const& c = *it;
        if (!readString(c, "gamestage", s.gamestage, error, "conditions")) return std::nullopt;
        if (!readString(c, "advancement", s.advancement, error, "conditions")) return std::nullopt;
        if (!readString(c, "xp", s.xp, error, "conditions")) return std::nullopt;
        if (!readString(c, "gamemode", s.gamemode, error, "conditions")) return std::nullopt;
        if (!readString(c, "scoreboard", s.scoreboard, error, "conditions")) return std::nullopt;
        if (!readString(c, "position", s.position, error, "conditions")) return std::nullopt;
        if (!readString(c, "effects", s.effects, error, "conditions")) return std::nullopt;
    }
    if (auto it = j.find("render"); it != j.end() && !it->is_null()) {
        if (!it->is_object()) return fail("render must be an object");
        hasRender = true;
        s.render  = *it;
    }
    if (auto it = j.find("effects"); it != j.end() && !it->is_null()) {
        if (!it->is_object()) return fail("effects must be an object");
        hasEffects    = true;
        auto const& e = *it;
        if (!readString(e, "commandPickup", s.commandPickup, error, "effects")) return std::nullopt;
        if (!readString(e, "commandLoop", s.commandLoop, error, "effects")) return std::nullopt;
        if (!readString(e, "commandPlace", s.commandPlace, error, "effects")) return std::nullopt;
    }
    if (!hasConditions && !hasRender && !hasEffects && !s.overrideChecks) {
        return fail("a script needs conditions, render, effects or \"override_checks\": true");
    }
    return s;
}

void sortScripts(std::vector<Script>& scripts) {
    std::stable_sort(scripts.begin(), scripts.end(), [](Script const& a, Script const& b) {
        return a.priority > b.priority;
    });
}

// ---- custom pickup conditions -----------------------------------------------------------------

std::optional<CustomCondition> parseCustomCondition(std::string_view text) {
    text       = trim(text);
    auto split = carry::splitSpec(text);
    auto rest  = trim(split.rest);
    if (rest.size() < 3 || rest.front() != '(' || rest.back() != ')') return std::nullopt;
    auto stage = trim(rest.substr(1, rest.size() - 2));
    if (stage.empty() || stage.find_first_of("()") != std::string_view::npos) return std::nullopt;
    auto entry = carry::parseEntry(split.spec);
    if (!entry) return std::nullopt;
    return CustomCondition{std::string(text), std::move(*entry), std::string(stage)};
}

std::string stageTag(std::string_view stage) { return "carryon:stage:" + std::string(stage); }

} // namespace lo::core::scripts
