#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace basalt::proxy::config {

inline constexpr std::size_t kMaxPatternBytes = 128;
inline constexpr std::size_t kMaxMatchedNameBytes = 32;

[[nodiscard]] std::optional<std::string_view> unsafePatternReason(std::string_view pattern);

} // namespace basalt::proxy::config
