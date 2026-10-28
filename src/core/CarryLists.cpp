#include "core/CarryLists.h"

#include <algorithm>
#include <cctype>

namespace lo::core::carry {

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

std::optional<bool> asBool(std::string_view v) {
    auto l = lower(v);
    if (l == "true" || l == "1") return true;
    if (l == "false" || l == "0") return false;
    return std::nullopt;
}
} // namespace

bool wildcardMatch(std::string_view pattern, std::string_view id) {
    std::size_t pos = 0;
    while (true) {
        auto star  = pattern.find('*');
        auto piece = pattern.substr(0, star);
        if (!piece.empty()) {
            auto at = id.find(piece, pos);
            if (at == std::string_view::npos) return false;
            pos = at + piece.size();
        }
        if (star == std::string_view::npos) return true;
        pattern.remove_prefix(star + 1);
    }
}

bool stateValueEquals(std::string_view expected, std::string_view actual) {
    if (expected == actual) return true;
    auto a = asBool(expected), b = asBool(actual);
    return a && b && *a == *b;
}

std::vector<StateFilter> parseStateFilters(std::string_view inner) {
    std::vector<StateFilter> out;
    while (!inner.empty()) {
        auto comma = inner.find(',');
        auto part  = trim(inner.substr(0, comma));
        inner      = comma == std::string_view::npos ? std::string_view{} : inner.substr(comma + 1);
        if (part.empty()) continue;
        auto eq = part.find('=');
        if (eq == std::string_view::npos) {
            out.push_back({std::string(part), std::nullopt});
        } else {
            out.push_back({std::string(trim(part.substr(0, eq))), std::string(trim(part.substr(eq + 1)))});
        }
    }
    return out;
}

SplitSpec splitSpec(std::string_view text) {
    text = trim(text);
    std::size_t i = 0;
    while (i < text.size() && text[i] != '[' && text[i] != '(' && text[i] != '{' && text.substr(i, 2) != "->") ++i;
    if (i < text.size() && text[i] == '[') {
        auto close = text.find(']', i);
        if (close == std::string_view::npos) return {text, {}};
        i = close + 1;
    }
    return {trim(text.substr(0, i)), trim(text.substr(i))};
}

std::optional<ListEntry> parseEntry(std::string_view text) {
    text = trim(text);
    if (text.empty()) return std::nullopt;
    ListEntry e;
    auto      open = text.find('[');
    if (open != std::string_view::npos) {
        if (text.back() != ']') return std::nullopt;
        auto inner = text.substr(open + 1, text.size() - open - 2);
        if (inner.find('[') != std::string_view::npos || inner.find(']') != std::string_view::npos) return std::nullopt;
        e.states = parseStateFilters(inner);
        text     = trim(text.substr(0, open));
    } else if (text.find(']') != std::string_view::npos) {
        return std::nullopt;
    }
    if (text.empty()) return std::nullopt;
    if (text.front() == '#') {
        text.remove_prefix(1);
        if (text.empty()) return std::nullopt;
        e.kind = ListEntry::Kind::Tag;
    } else if (text.find('*') != std::string_view::npos) {
        e.kind = ListEntry::Kind::Wildcard;
        auto p = text;
        while (true) {
            auto star = p.find('*');
            if (auto piece = p.substr(0, star); !piece.empty()) e.pieces.emplace_back(piece);
            if (star == std::string_view::npos) break;
            p.remove_prefix(star + 1);
        }
    }
    e.text = std::string(text);
    return e;
}

bool ListEntry::matchesId(Candidate const& c) const {
    switch (kind) {
    case Kind::Id:
        return c.id == text;
    case Kind::Wildcard:
        return wildcardMatch(text, c.id);
    case Kind::Tag:
        return c.hasTag && c.hasTag(text);
    }
    return false;
}

bool ListEntry::matches(Candidate const& c) const {
    if (!matchesId(c)) return false;
    for (auto const& f : states) {
        if (!f.value) continue;
        if (!c.state) return false;
        auto actual = c.state(f.name);
        if (!actual || !stateValueEquals(*f.value, *actual)) return false;
    }
    return true;
}

IdList::IdList(std::vector<std::string> const& entries, std::vector<std::string>* invalid) {
    mEntries.reserve(entries.size());
    for (auto const& text : entries) {
        if (auto e = parseEntry(text)) mEntries.push_back(std::move(*e));
        else if (invalid && !trim(text).empty()) invalid->push_back(text);
    }
}

bool IdList::matches(Candidate const& c) const {
    return std::any_of(mEntries.begin(), mEntries.end(), [&](ListEntry const& e) { return e.matches(c); });
}

bool isAllowed(IdList const& blacklist, IdList const& whitelist, bool useWhitelist, Candidate const& c) {
    if (useWhitelist) return whitelist.matches(c);
    return !blacklist.matches(c);
}

StateExceptions::StateExceptions(std::vector<std::string> const& entries, std::vector<std::string>* invalid) {
    for (auto const& text : entries) {
        auto e = parseEntry(text);
        if (!e || e->states.empty()) {
            if (invalid && !trim(text).empty()) invalid->push_back(text);
            continue;
        }
        for (auto const& s : e->states) {
            if (std::find(mNames.begin(), mNames.end(), s.name) == mNames.end()) mNames.push_back(s.name);
        }
    }
}

bool StateExceptions::contains(std::string_view stateName) const {
    return std::find(mNames.begin(), mNames.end(), stateName) != mNames.end();
}

} // namespace lo::core::carry
