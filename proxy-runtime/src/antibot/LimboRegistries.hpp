#pragma once

#include <span>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime::antibot {

struct RegistryNames {
    std::string_view registry;
    std::vector<std::string_view> entries;
};

[[nodiscard]] std::span<const RegistryNames> limboRegistries();

} // namespace basalt::proxy::runtime::antibot
