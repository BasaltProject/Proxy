#include "TextScan.hpp"

#include <algorithm>
#include <cctype>

namespace basalt::proxy::runtime::antibot {
namespace {
std::string lowered(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
} // namespace

TextScan::TextScan(const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        std::vector<std::string> segments;
        std::size_t begin = 0;
        while (begin <= pattern.size()) {
            const auto star = pattern.find('*', begin);
            const auto end = star == std::string::npos ? pattern.size() : star;
            if (end > begin)
                segments.push_back(lowered(std::string_view{pattern}.substr(begin, end - begin)));
            if (star == std::string::npos)
                break;
            begin = star + 1;
        }
        if (!segments.empty())
            patterns_.push_back(std::move(segments));
    }
}

bool TextScan::matches(std::string_view text) const {
    const auto haystack = lowered(text);
    return std::ranges::any_of(patterns_, [&](const std::vector<std::string>& segments) {
        std::size_t from = 0;
        for (const auto& segment : segments) {
            const auto found = haystack.find(segment, from);
            if (found == std::string::npos)
                return false;
            from = found + segment.size();
        }
        return true;
    });
}

} // namespace basalt::proxy::runtime::antibot
