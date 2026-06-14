#pragma once

#include <basalt/proxy/plugins/AdapterMapping.hpp>

#include <span>

namespace basalt::proxy::velocity {

/// Velocity API member to native service and Lua spelling; the adapter reads this, it adds no behavior.
[[nodiscard]] std::span<const plugins::AdapterMapping> mappings() noexcept;

} // namespace basalt::proxy::velocity
