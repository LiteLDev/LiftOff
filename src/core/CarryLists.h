#pragma once

// Carry On list entries (black/whitelists, custom pickup conditions, placement state exceptions, model
// overrides), game independent. See docs/SPEC.md §5.3.
//
//   ns:id                 exact id
//   ns:*_door             wildcard: split on '*', every piece must occur in the id in order (substring)
//   #tag                  tag: block tag (blocks) / type family (entities), answered by the caller
//   ns:id[state=value]    [LO] blocks: the listed states must have these values (Bedrock states)
//   ns:id[state,state]    a list of state names (placement exceptions, model overrides)

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lo::core::carry {

/// `name` or `name=value` inside `[...]`.
struct StateFilter {
    std::string                name;
    std::optional<std::string> value; ///< nullopt: the entry only names the state

    friend bool operator==(StateFilter const&, StateFilter const&) = default;
};

/// The thing a list entry is matched against. `hasTag` / `state` may be empty (no tags / no states).
struct Candidate {
    std::string_view                                                id;
    std::function<bool(std::string_view tag)>                       hasTag;
    /// The candidate's value of a state as text (ints in decimal, bools "true"/"false"), nullopt if
    /// the candidate has no such state.
    std::function<std::optional<std::string>(std::string_view name)> state;
};

struct ListEntry {
    enum class Kind { Id, Tag, Wildcard };
    Kind                     kind = Kind::Id;
    std::string              text;   ///< id, tag name (without '#') or wildcard pattern
    std::vector<std::string> pieces; ///< wildcard pieces (non-empty), in order
    std::vector<StateFilter> states; ///< `[...]` filters

    /// Id / wildcard / tag test only (ignores `states`).
    [[nodiscard]] bool matchesId(Candidate const& c) const;
    /// Id test plus every `[state=value]` filter (filters without a value are ignored).
    [[nodiscard]] bool matches(Candidate const& c) const;
};

/// Carry On wildcard: the pattern split on '*'; every non-empty piece must occur in `id`, in order.
[[nodiscard]] bool wildcardMatch(std::string_view pattern, std::string_view id);

/// Parses the `[a,b=c]` part (without brackets). Whitespace around names/values is trimmed.
[[nodiscard]] std::vector<StateFilter> parseStateFilters(std::string_view inner);

/// Parses one entry. Leading/trailing whitespace is ignored. Returns nullopt for empty or malformed
/// entries (unbalanced brackets, empty id).
[[nodiscard]] std::optional<ListEntry> parseEntry(std::string_view text);

/// Splits `text` into an id part and its trailing parts: "id[states]rest" -> {"id[states]", "rest"}.
/// Used by formats that append something (custom conditions "(stage)", model overrides "{nbt}->...").
struct SplitSpec {
    std::string_view spec; ///< id with an optional `[...]`
    std::string_view rest; ///< everything after it
};
[[nodiscard]] SplitSpec splitSpec(std::string_view text);

/// A compiled list (blacklist or whitelist).
class IdList {
public:
    IdList() = default;
    /// Invalid entries are skipped; their text is appended to `invalid` when given.
    explicit IdList(std::vector<std::string> const& entries, std::vector<std::string>* invalid = nullptr);

    [[nodiscard]] bool                          matches(Candidate const& c) const;
    [[nodiscard]] bool                          empty() const { return mEntries.empty(); }
    [[nodiscard]] std::vector<ListEntry> const& entries() const { return mEntries; }

private:
    std::vector<ListEntry> mEntries;
};

/// Carry On list semantics: in whitelist mode only whitelisted ids are allowed (the blacklist is
/// ignored); otherwise everything not blacklisted is allowed.
[[nodiscard]] bool isAllowed(IdList const& blacklist, IdList const& whitelist, bool useWhitelist, Candidate const& c);

/// `placementStateExceptions`: `id[state,state]` entries. Like Carry On (which matches the Java property
/// object), a named state applies to every block that has a state of that name.
class StateExceptions {
public:
    StateExceptions() = default;
    explicit StateExceptions(std::vector<std::string> const& entries, std::vector<std::string>* invalid = nullptr);

    [[nodiscard]] bool contains(std::string_view stateName) const;
    [[nodiscard]] std::vector<std::string> const& names() const { return mNames; }

private:
    std::vector<std::string> mNames;
};

/// Compares a state value from a list entry with the candidate's value: exact text, or bool aliases
/// ("true"/"1", "false"/"0"), case-insensitive for bools.
[[nodiscard]] bool stateValueEquals(std::string_view expected, std::string_view actual);

} // namespace lo::core::carry
