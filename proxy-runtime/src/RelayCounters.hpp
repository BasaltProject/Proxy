#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace basalt::proxy::runtime::detail {

/// Process-wide, relaxed: sessions on every owner thread add to them and the console reads them.
struct RelayCounters {
    std::atomic<std::uint64_t> towardClients{0};
    std::atomic<std::uint64_t> towardBackends{0};
    std::atomic<std::uint64_t> logins{0};
    std::atomic<std::uint64_t> backendChanges{0};
    /// Sessions that ended while in the handshake or status, login, configuration or play.
    std::array<std::atomic<std::uint64_t>, 4> endedIn{};
};

[[nodiscard]] inline RelayCounters& relayCounters() noexcept {
    static RelayCounters counters;
    return counters;
}

} // namespace basalt::proxy::runtime::detail
