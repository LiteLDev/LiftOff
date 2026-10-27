#include "core/Format.h"

namespace lo::core {

std::string formatMessage(std::string_view fmt, std::span<std::string const> args) {
    std::string out;
    out.reserve(fmt.size() + 16);
    std::size_t next = 0;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        char c = fmt[i];
        if (c == '{' && i + 1 < fmt.size() && fmt[i + 1] == '{') {
            out += '{';
            ++i;
            continue;
        }
        if (c == '}' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
            out += '}';
            ++i;
            continue;
        }
        if (c == '{') {
            auto close = fmt.find('}', i);
            if (close != std::string_view::npos) {
                auto inner = fmt.substr(i + 1, close - i - 1);
                bool digits = !inner.empty();
                std::size_t idx = 0;
                for (char d : inner) {
                    if (d < '0' || d > '9') {
                        digits = false;
                        break;
                    }
                    idx = idx * 10 + std::size_t(d - '0');
                }
                if (inner.empty() && next < args.size()) {
                    out += args[next++];
                    i = close;
                    continue;
                }
                if (digits && idx < args.size()) {
                    out += args[idx];
                    i = close;
                    continue;
                }
            }
        }
        out += c;
    }
    return out;
}

std::string compactCount(long long n) {
    if (n < 0) return "-" + compactCount(-n);
    if (n < 1000) return std::to_string(n);
    auto fmt1 = [](long long v, long long unit, char suffix) {
        long long tenths = v * 10 / unit;
        std::string s    = std::to_string(tenths / 10);
        if (tenths < 100 && tenths % 10 != 0) s += "." + std::to_string(tenths % 10);
        return s + suffix;
    };
    if (n < 1000000) return fmt1(n, 1000, 'k');
    if (n < 1000000000) return fmt1(n, 1000000, 'M');
    return fmt1(n, 1000000000, 'G');
}

} // namespace lo::core
