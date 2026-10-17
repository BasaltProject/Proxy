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
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::config {
struct ProxyConfig;
}

namespace basalt::proxy::runtime {

enum class PlayerEventKind { kJoin, kLeave, kSwitch, kChat, kKick, kPing, kLogin };

struct PlayerEvent {
    PlayerEventKind kind;
    std::string player;
    /// The backend the player is on; for a leave, the one it left from.
    std::string backend;
    std::string uuid;
    /// The backend a switch came from; for a chat, the message.
    std::string from;
};

/// A command a plugin registered for players: the words that name it, then typed arguments.
struct PlayerCommandSpec {
    enum class Kind : std::uint8_t { kWord, kText, kInt, kFloat, kBool };

    struct Argument {
        std::string name;
        Kind kind = Kind::kWord;
        bool optional = false;
        std::optional<std::int64_t> min;
        std::optional<std::int64_t> max;
    };

    std::vector<std::string> literals;
    std::vector<Argument> arguments;
};

struct RuntimeServices {
    wire::auth::AuthenticationFetch authentication;
    std::function<std::expected<std::vector<wire::io::TcpAddress>, wire::io::DnsError>(std::string_view, std::uint16_t,
                                                                                       const wire::io::DnsOptions&)>
        resolution = {};
    /// Called on session owner threads; must return quickly and never block.
    std::function<void(PlayerEvent)> playerEvents = {};
    /// Offered every command typed in play; true keeps it from the backend. Runs on owner threads and must not block.
    std::function<bool(std::string_view player, std::string_view command)> playerCommand = {};
    /// Told every chat message a player sends, after the fact. Called on session owner threads; must not block.
    std::function<void(std::string_view player, std::string_view message)> playerChat = {};
    /// Told, with its plain text, why a backend disconnected a player in play. Called on session owner threads.
    std::function<void(std::string_view player, std::string_view reason)> playerKick = {};
    /// Told the host and protocol of every server-list request answered. Called on session owner threads.
    std::function<void(std::string_view host, std::int32_t protocol)> statusPing = {};
    /// Told a player's name and uuid once they are authenticated, before any backend is chosen.
    std::function<void(std::string_view player, std::string_view uuid)> playerLogin = {};
    /// Those commands, added to every command tree the proxy relays so clients know them and complete their arguments.
    std::function<std::vector<PlayerCommandSpec>()> playerCommandSpecs = {};
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
    /// Thread-safe. Validates first, then swaps backends and routes atomically; fixed settings need a restart.
    [[nodiscard]] std::expected<std::size_t, std::string> reload(config::ProxyConfig fresh);
    [[nodiscard]] std::vector<std::string> backendNames() const;
    /// Thread-safe. Queues a chat line for a player; true once handed to the session owners.
    [[nodiscard]] bool tellPlayer(std::string_view player, std::string_view text);
    /// Thread-safe. Disconnects a player in play with a MiniMessage reason; true when handed to the session owners.
    [[nodiscard]] bool kickPlayer(std::string_view player, std::string_view reason);
    [[nodiscard]] std::uint16_t port() const noexcept;
    /// Counts admitted sessions, including bounded handoffs waiting for their owner.
    [[nodiscard]] std::size_t sessionCount() const noexcept;
    [[nodiscard]] std::size_t bufferedBytes() const noexcept;
    /// Connections admitted to a session owner, and those turned away by rate limits or exhausted budgets.
    [[nodiscard]] std::uint64_t acceptedConnections() const noexcept;
    [[nodiscard]] std::uint64_t rejectedConnections() const noexcept;
    /// Packet bytes queued toward clients and backends since the process started.
    [[nodiscard]] std::uint64_t bytesTowardClients() const noexcept;
    [[nodiscard]] std::uint64_t bytesTowardBackends() const noexcept;

private:
    struct Impl;
    explicit ProxyRuntime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace basalt::proxy::runtime
