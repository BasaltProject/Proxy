#pragma once

#include <atomic>
#include <cstdint>

namespace basalt::proxy::runtime::detail {

/// Process-wide, relaxed: sessions on every owner thread add to them and the console reads them.
struct RelayCounters {
    std::atomic<std::uint64_t> towardClients{0};
    std::atomic<std::uint64_t> towardBackends{0};
};

[[nodiscard]] inline RelayCounters& relayCounters() noexcept {
    static RelayCounters counters;
    return counters;
}

} // namespace basalt::proxy::runtime::detail
