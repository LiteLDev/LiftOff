#include "core/ModelOverride.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>

namespace lo::core::modeloverride {

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

bool startsWithIgnoreCase(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && lower(s.substr(0, prefix.size())) == prefix;
}

/// Characters an id may not contain (ids are `ns:path`; wildcards / tags are list syntax).
bool plainId(std::string_view id) {
    if (id.empty()) return false;
    return std::none_of(id.begin(), id.end(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) || c == '*' || c == '#' || c == '[' || c == ']' || c == '{'
            || c == '}' || c == '(' || c == ')' || c == '"' || c == '\'' || c == ',' || c == '=';
    });
}

/// Index of the bracket closing the one at `open` (`{`/`[`), skipping nested brackets and quoted
/// strings; npos if unbalanced.
std::size_t findClosing(std::string_view s, std::size_t open) {
    std::vector<char> stack;
    char              quote = 0;
    for (std::size_t i = open; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        switch (c) {
        case '"':
        case '\'':
            quote = c;
            break;
        case '{':
            stack.push_back('}');
            break;
        case '[':
            stack.push_back(']');
            break;
        case '}':
        case ']':
            if (stack.empty() || stack.back() != c) return std::string_view::npos;
            stack.pop_back();
            if (stack.empty()) return i;
            break;
        default:
            break;
        }
    }
    return std::string_view::npos;
}

/// Position of the `->` separating source and target (outside brackets and quotes); npos if none.
std::size_t findArrow(std::string_view s) {
    int  depth = 0;
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
        else if (depth == 0 && c == '-' && i + 1 < s.size() && s[i + 1] == '>') return i;
    }
    return std::string_view::npos;
}

bool allValued(std::vector<carry::StateFilter> const& states) {
    return std::all_of(states.begin(), states.end(), [](carry::StateFilter const& f) {
        return !f.name.empty() && f.value && !f.value->empty();
    });
}

std::optional<float> parseFloat(std::string_view s) {
    s = trim(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return std::nullopt;
    float v   = 0.f;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return v;
}

std::optional<std::string> nonEmptyString(nlohmann::json const& obj, char const* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return std::nullopt;
    auto s = trim(it->get_ref<std::string const&>());
    if (s.empty()) return std::nullopt;
    return std::string(s);
}

std::optional<bool> boolValue(nlohmann::json const& obj, char const* key) {
    auto it = obj.find(key);
    if (it == obj.end()) return std::nullopt;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) {
        auto l = lower(trim(it->get_ref<std::string const&>()));
        if (l == "true") return true;
        if (l == "false") return false;
    }
    return std::nullopt;
}

bool simpleKey(std::string_view k) {
    return !k.empty() && std::all_of(k.begin(), k.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '+';
    });
}

void appendQuoted(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
}

void appendSnbt(std::string& out, nlohmann::json const& v) {
    using value_t = nlohmann::json::value_t;
    switch (v.type()) {
    case value_t::object: {
        out += '{';
        bool first = true;
        for (auto const& [k, item] : v.items()) {
            if (!first) out += ',';
            first = false;
            if (simpleKey(k)) out += k;
            else appendQuoted(out, k);
            out += ':';
            appendSnbt(out, item);
        }
        out += '}';
        break;
    }
    case value_t::array: {
        out += '[';
        bool first = true;
        for (auto const& item : v) {
            if (!first) out += ',';
            first = false;
            appendSnbt(out, item);
        }
        out += ']';
        break;
    }
    case value_t::string:
        appendQuoted(out, v.get_ref<std::string const&>());
        break;
    case value_t::boolean:
        out += v.get<bool>() ? "1b" : "0b";
        break;
    case value_t::number_integer: {
        auto n = v.get<std::int64_t>();
        out += std::to_string(n);
        if (n < std::numeric_limits<std::int32_t>::min() || n > std::numeric_limits<std::int32_t>::max()) out += 'L';
        break;
    }
    case value_t::number_unsigned: {
        auto n = v.get<std::uint64_t>();
        if (n <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            out += std::to_string(n);
        } else if (n <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            out += std::to_string(n);
            out += 'L';
        } else {
            out += std::to_string(static_cast<double>(n));
            out += 'd';
        }
        break;
    }
    case value_t::number_float: {
        char buf[64];
        auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), v.get<double>());
        std::string_view num(buf, ec == std::errc{} ? static_cast<std::size_t>(p - buf) : 0);
        if (num.empty() || num == "inf" || num == "-inf" || num == "nan" || num == "-nan") num = "0";
        out += num;
        out += 'd';
        break;
    }
    default:
        out += "0b";
        break;
    }
}
} // namespace

std::string withNamespace(std::string_view id) {
    id = trim(id);
    if (id.empty() || id.find(':') != std::string_view::npos) return std::string(id);
    return "minecraft:" + std::string(id);
}

std::string ModelOverride::targetText() const {
    std::string out = kind == TargetKind::Item ? "(item)" : "(block)";
    out += target;
    if (!targetStates.empty()) {
        out += '[';
        for (std::size_t i = 0; i < targetStates.size(); ++i) {
            if (i) out += ',';
            out += targetStates[i].name;
            out += '=';
            out += targetStates[i].value.value_or("");
        }
        out += ']';
    }
    return out;
}

std::optional<BlockSpec> parseBlockSpec(std::string_view text) {
    text = trim(text);
    BlockSpec spec;
    if (auto open = text.find('['); open != std::string_view::npos) {
        if (findClosing(text, open) != text.size() - 1) return std::nullopt;
        auto inner = text.substr(open + 1, text.size() - open - 2);
        if (inner.find('[') != std::string_view::npos || inner.find(']') != std::string_view::npos) return std::nullopt;
        spec.states = carry::parseStateFilters(inner);
        if (!allValued(spec.states)) return std::nullopt;
        text = trim(text.substr(0, open));
    }
    if (!plainId(text)) return std::nullopt;
    spec.id = withNamespace(text);
    return spec;
}

std::optional<ModelOverride> parse(std::string_view text) {
    text = trim(text);
    auto arrow = findArrow(text);
    if (arrow == std::string_view::npos) return std::nullopt;
    auto left  = trim(text.substr(0, arrow));
    auto right = trim(text.substr(arrow + 2));

    ModelOverride o;
    o.text = std::string(text);

    // Source: id[states]{nbt}
    auto split = carry::splitSpec(left);
    auto rest  = split.rest;
    if (!rest.empty()) {
        if (rest.front() != '{' || findClosing(rest, 0) != rest.size() - 1) return std::nullopt;
        o.nbt = std::string(rest);
    }
    auto entry = carry::parseEntry(split.spec);
    if (!entry || !allValued(entry->states)) return std::nullopt;
    if (entry->kind == carry::ListEntry::Kind::Id) {
        if (!plainId(entry->text)) return std::nullopt;
        entry->text = withNamespace(entry->text);
    } else if (entry->kind == carry::ListEntry::Kind::Wildcard) {
        if (entry->text.find_first_of("{}()") != std::string::npos) return std::nullopt;
    }
    o.source = std::move(*entry);

    // Target: (item|block)id[states]
    if (startsWithIgnoreCase(right, "(item)")) {
        o.kind = TargetKind::Item;
        right  = trim(right.substr(6));
    } else if (startsWithIgnoreCase(right, "(block)")) {
        o.kind = TargetKind::Block;
        right  = trim(right.substr(7));
    } else if (!right.empty() && right.front() == '(') {
        return std::nullopt;
    }
    if (o.kind == TargetKind::Item) {
        if (!plainId(right)) return std::nullopt;
        o.target = withNamespace(right);
    } else {
        auto spec = parseBlockSpec(right);
        if (!spec) return std::nullopt;
        o.target       = std::move(spec->id);
        o.targetStates = std::move(spec->states);
    }
    return o;
}

std::vector<ModelOverride> parseAll(std::vector<std::string> const& entries, std::vector<std::string>* invalid) {
    std::vector<ModelOverride> out;
    out.reserve(entries.size());
    for (auto const& text : entries) {
        if (auto o = parse(text)) out.push_back(std::move(*o));
        else if (invalid && !trim(text).empty()) invalid->push_back(text);
    }
    return out;
}

Axes parseAxes(std::string_view text) {
    Axes out;
    while (!text.empty()) {
        auto comma = text.find(',');
        auto part  = trim(text.substr(0, comma));
        text       = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        auto eq    = part.find('=');
        if (eq == std::string_view::npos) continue;
        auto key = lower(trim(part.substr(0, eq)));
        auto val = parseFloat(part.substr(eq + 1));
        if (!val) continue;
        if (key == "x") out[0] = val;
        else if (key == "y") out[1] = val;
        else if (key == "z") out[2] = val;
    }
    return out;
}

std::array<float, 3> parseVector(std::string_view text, float fallback) {
    auto axes = parseAxes(text);
    return {axes[0].value_or(fallback), axes[1].value_or(fallback), axes[2].value_or(fallback)};
}

bool ScriptRender::empty() const {
    auto unset = [](Axes const& a) { return !a[0] && !a[1] && !a[2]; };
    return !nameBlock && !nameEntity && !nbt && translation == std::array{0.f, 0.f, 0.f}
        && rotation == std::array{0.f, 0.f, 0.f} && scale == std::array{1.f, 1.f, 1.f} && unset(leftArm)
        && unset(rightArm) && renderLeftArm && renderRightArm;
}

ScriptRender parseScriptRender(nlohmann::json const& render) {
    ScriptRender r;
    if (!render.is_object()) return r;
    if (auto s = nonEmptyString(render, "name_block")) {
        if (auto spec = parseBlockSpec(*s)) {
            std::string text = spec->id;
            if (!spec->states.empty()) {
                text += '[';
                for (std::size_t i = 0; i < spec->states.size(); ++i) {
                    if (i) text += ',';
                    text += spec->states[i].name + "=" + *spec->states[i].value;
                }
                text += ']';
            }
            r.nameBlock = std::move(text);
        }
    }
    if (auto s = nonEmptyString(render, "name_entity"); s && plainId(*s)) r.nameEntity = withNamespace(*s);
    if (auto it = render.find("nbt"); it != render.end()) {
        if (it->is_object()) {
            if (!it->empty()) r.nbt = jsonToSnbt(*it);
        } else if (it->is_string()) {
            auto s = trim(it->get_ref<std::string const&>());
            if (s.size() > 2 && s.front() == '{' && findClosing(s, 0) == s.size() - 1) r.nbt = std::string(s);
        }
    }
    if (auto s = nonEmptyString(render, "translation")) r.translation = parseVector(*s, 0.f);
    if (auto s = nonEmptyString(render, "rotation")) r.rotation = parseVector(*s, 0.f);
    if (auto s = nonEmptyString(render, "scale")) r.scale = parseVector(*s, 1.f);
    if (auto s = nonEmptyString(render, "rotation_left_arm")) r.leftArm = parseAxes(*s);
    if (auto s = nonEmptyString(render, "rotation_right_arm")) r.rightArm = parseAxes(*s);
    if (auto b = boolValue(render, "render_left_arm")) r.renderLeftArm = *b;
    if (auto b = boolValue(render, "render_right_arm")) r.renderRightArm = *b;
    return r;
}

std::string jsonToSnbt(nlohmann::json const& value) {
    std::string out;
    appendSnbt(out, value);
    return out;
}

} // namespace lo::core::modeloverride
