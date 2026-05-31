#pragma once

#include <basalt/proxy/runtime/BackendChangeRequest.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/common/placeholder/Placeholders.hpp>
#include <basalt/wire/config/UiConfigs.hpp>
#include <basalt/wire/io/DnsResolver.hpp>
#include <basalt/wire/io/TcpSocket.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>

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
        /// The client asks the proxy for completions of this argument.
        bool suggest = false;
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
    /// Told a player's name and uuid once they are authenticated, before any backend is chosen. A returned MiniMessage
    /// turns the player away with that text.
    std::function<std::optional<std::string>(std::string_view player, std::string_view uuid, std::string_view address)>
        playerLogin = {};
    /// The text of one of the proxy's own refusals by its messages.toml key; empty keeps the built-in English one.
    std::function<std::string(std::string_view key)> kickText = {};
    /// Those commands for one player, added to every command tree the proxy relays so clients know them.
    std::function<std::vector<PlayerCommandSpec>(std::string_view player)> playerCommandSpecs = {};
    /// Completions for the last word of a command line a player is typing, when it starts with a proxy command.
    std::function<std::optional<std::vector<std::string>>(std::string_view player, std::string_view line)>
        commandSuggestions = {};
    /// The server list answer for one ping; the proxy's own one-line answer when unset. Runs on owner threads.
    /// MiniMessage text a login is turned away with while the proxy is still starting; empty once it is ready.
    std::function<std::optional<std::string>()> loadingMessage = {};
    /// The tab list, boss bar and scoreboard the proxy draws; unset draws nothing. Runs on owner threads.
    std::function<std::shared_ptr<const wire::config::UiConfigs>()> uiConfigs = {};
    const wire::common::placeholder::PlaceholderRegistry* placeholders = nullptr;
    std::function<wire::protocol::status::ServerStatusPayload(std::int32_t protocol, std::string_view host,
                                                              std::string_view address)>
        status = {};
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
