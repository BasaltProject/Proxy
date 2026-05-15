#pragma once

#include <cstdint>
#include <vector>

namespace basalt::proxy::runtime::detail {

/// A 64x64 PNG of basalt columns, drawn by code so no image file ships with the proxy.
[[nodiscard]] std::vector<std::uint8_t> defaultIcon();

} // namespace basalt::proxy::runtime::detail
