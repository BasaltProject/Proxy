#pragma once

#include <basalt/wire/protocol/status/StatusPackets.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>

namespace basalt::proxy::runtime::detail {

struct ProbeTarget {
    std::string host;
    std::uint16_t port = 0;
    std::int32_t protocol = 0;
};

/// Blocking checks for a dedicated thread, each bounded by its timeout.
[[nodiscard]] bool tcpReachable(const ProbeTarget& target, std::chrono::milliseconds timeout,
                                const std::stop_token& stop);

/// The server's multiplayer-list status, asked the way a client asks; empty when it does not answer in time.
[[nodiscard]] std::optional<wire::protocol::status::ServerStatusPayload>
statusPing(const ProbeTarget& target, std::chrono::milliseconds timeout, const std::stop_token& stop);

} // namespace basalt::proxy::runtime::detail
