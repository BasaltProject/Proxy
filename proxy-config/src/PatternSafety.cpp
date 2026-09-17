#include <basalt/proxy/config/PatternSafety.hpp>

#include <vector>

namespace basalt::proxy::config {
namespace {

struct Group {
    bool repeats = false;
    bool alternates = false;
};

[[nodiscard]] bool quantifierAt(std::string_view text, std::size_t at) {
    if (at >= text.size())
        return false;
    return text[at] == '*' || text[at] == '+' ||
           (text[at] == '{' && at + 1 < text.size() && text[at + 1] >= '0' && text[at + 1] <= '9');
}

} // namespace

std::optional<std::string_view> unsafePatternReason(std::string_view pattern) {
    if (pattern.size() > kMaxPatternBytes)
        return "is longer than 128 bytes";
    std::vector<Group> groups;
    bool inClass = false;
    for (std::size_t at = 0; at < pattern.size(); ++at) {
        const char c = pattern[at];
        if (c == '\\') {
            if (at + 1 < pattern.size() && pattern[at + 1] >= '1' && pattern[at + 1] <= '9')
                return "uses a back-reference";
            ++at;
            continue;
        }
        if (inClass) {
            inClass = c != ']';
            continue;
        }
        switch (c) {
        case '[':
            inClass = true;
            break;
        case '(':
            groups.emplace_back();
            break;
        case '|':
            if (!groups.empty())
                groups.back().alternates = true;
            break;
        case '*':
        case '+':
        case '{':
            if (c != '{' || quantifierAt(pattern, at)) {
                if (!groups.empty())
                    groups.back().repeats = true;
            }
            break;
        case ')': {
            if (groups.empty())
                break;
            const Group closed = groups.back();
            groups.pop_back();
            if (quantifierAt(pattern, at + 1) && (closed.repeats || closed.alternates))
                return "repeats a group that already repeats or alternates";
            if (!groups.empty()) {
                groups.back().repeats = groups.back().repeats || closed.repeats;
                groups.back().alternates = groups.back().alternates || closed.alternates;
            }
            break;
        }
        default:
            break;
        }
    }
    return std::nullopt;
}

} // namespace basalt::proxy::config
