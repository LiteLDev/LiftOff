#pragma once

#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lo::core {

/// Formats a translation string. Supports `{}` (next argument), `{0}`, `{1}`... (positional),
/// and `{{` / `}}` escapes. Never throws: unknown placeholders are kept verbatim.
[[nodiscard]] std::string formatMessage(std::string_view fmt, std::span<std::string const> args);

template <class T>
[[nodiscard]] std::string toArgString(T const& v) {
    if constexpr (std::is_convertible_v<T const&, std::string_view>) return std::string(std::string_view(v));
    else return std::format("{}", v);
}

template <class... Args>
[[nodiscard]] std::string formatMessageArgs(std::string_view fmt, Args const&... args) {
    std::vector<std::string> v{toArgString(args)...};
    return formatMessage(fmt, v);
}

/// Formats a large count compactly: 999 -> "999", 1234 -> "1.2k", 1500000 -> "1.5M".
[[nodiscard]] std::string compactCount(long long n);

} // namespace lo::core
