#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace basalt::proxy::runtime {

/// Process-wide relay counters since start; sessions ended while in the handshake or status, login, configuration,
/// play.
struct RelayStats {
    std::uint64_t logins = 0;
    std::uint64_t backendChanges = 0;
    std::array<std::uint64_t, 4> endedIn{};
};

[[nodiscard]] RelayStats relayStats() noexcept;

struct ProtocolSupport {
    std::int32_t protocol = 0;
    std::string name;
    /// Exercised against a real client or server, not only present in the tables.
    bool verified = false;
};

/// Client protocols that may connect, the native one included.
[[nodiscard]] std::vector<ProtocolSupport> clientProtocols();

/// Backend protocols the proxy can speak to, the native one included.
[[nodiscard]] std::vector<ProtocolSupport> backendProtocols();

} // namespace basalt::proxy::runtime
