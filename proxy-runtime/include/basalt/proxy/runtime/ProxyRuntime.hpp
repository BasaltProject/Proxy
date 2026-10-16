#pragma once

#include <basalt/proxy/runtime/BackendChangeRequest.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/io/DnsResolver.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::config {
struct ProxyConfig;
}

namespace basalt::proxy::runtime {

enum class PlayerEventKind { kJoin, kLeave, kSwitch };

struct PlayerEvent {
    PlayerEventKind kind;
    std::string player;
    std::string backend;
};

struct RuntimeServices {
    wire::auth::AuthenticationFetch authentication;
    std::function<std::expected<std::vector<wire::io::TcpAddress>, wire::io::DnsError>(std::string_view, std::uint16_t,
                                                                                       const wire::io::DnsOptions&)>
        resolution = {};
    /// Called on session owner threads; must return quickly and never block.
    std::function<void(PlayerEvent)> playerEvents = {};
};

class ProxyRuntime {
public:
    [[nodiscard]] static std::expected<ProxyRuntime, std::string> create(config::ProxyConfig config,
                                                                         RuntimeServices services = {});
    ProxyRuntime(ProxyRuntime&&) noexcept;
    ProxyRuntime& operator=(ProxyRuntime&&) noexcept;
    ~ProxyRuntime();
    ProxyRuntime(const ProxyRuntime&) = delete;
    ProxyRuntime& operator=(const ProxyRuntime&) = delete;

    /// Coordinator only; starts owners and joins them after stop or a fatal transport failure.
    [[nodiscard]] std::expected<void, std::string> run(std::stop_token stop);
    /// Thread-safe; matches the verified name exactly. Ticket retains bounded resources until released.
    [[nodiscard]] std::expected<BackendChangeRequest, BackendChangeError> sendPlayer(std::string_view player,
                                                                                     std::string_view backend);
    /// Thread-safe. Validates everything first, then swaps backends, routes and forced hosts atomically.
    /// Sessions keep the snapshot they started with; fixed settings (network, limits, forwarding) need a restart.
    [[nodiscard]] std::expected<std::size_t, std::string> reload(config::ProxyConfig fresh);
    [[nodiscard]] std::vector<std::string> backendNames() const;
    [[nodiscard]] std::uint16_t port() const noexcept;
    /// Counts admitted sessions, including bounded handoffs waiting for their owner.
    [[nodiscard]] std::size_t sessionCount() const noexcept;
    [[nodiscard]] std::size_t bufferedBytes() const noexcept;
    /// Connections admitted to a session owner, and those turned away by rate limits or exhausted budgets.
    [[nodiscard]] std::uint64_t acceptedConnections() const noexcept;
    [[nodiscard]] std::uint64_t rejectedConnections() const noexcept;

private:
    struct Impl;
    explicit ProxyRuntime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace basalt::proxy::runtime
