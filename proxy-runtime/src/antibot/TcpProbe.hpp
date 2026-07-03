#pragma once

#include <cstdint>
#include <optional>

namespace basalt::proxy::runtime::antibot {

struct TcpFacts {
    std::uint32_t rttMicros = 0;
    std::uint32_t mss = 0;
    std::uint32_t retransmits = 0;
};

/// The kernel's view of a connected socket; empty off Linux or when the call fails.
[[nodiscard]] std::optional<TcpFacts> probeTcp(std::int64_t handle);

} // namespace basalt::proxy::runtime::antibot
