#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime::antibot {

/// Case-insensitive patterns where `*` matches any run of characters, searched anywhere in a text, in linear time.
class TextScan {
public:
    TextScan() = default;
    explicit TextScan(const std::vector<std::string>& patterns);

    [[nodiscard]] bool matches(std::string_view text) const;

    [[nodiscard]] bool empty() const noexcept { return patterns_.empty(); }

private:
    std::vector<std::vector<std::string>> patterns_;
};

} // namespace basalt::proxy::runtime::antibot
