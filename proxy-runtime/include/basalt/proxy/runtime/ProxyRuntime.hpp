#pragma once

#include <basalt/proxy/runtime/BackendChangeRequest.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/auth/SignatureValidator.hpp>
#include <basalt/wire/common/placeholder/Placeholders.hpp>
#include <basalt/wire/config/UiConfigs.hpp>
#include <basalt/wire/io/DnsResolver.hpp>
#include <basalt/wire/io/TcpSocket.hpp>
#include <basalt/wire/plugins/PluginPackets.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>
#include <basalt/wire/text/Component.hpp>

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace basalt::proxy::config {
struct ProxyConfig;
}

namespace basalt::proxy::runtime {

enum class PlayerEventKind { kJoin, kLeave, kSwitch, kChat, kKick, kPing, kLogin, kLatency, kAntibot };

struct PlayerEvent {
    PlayerEventKind kind;
    std::string player;
    /// The backend the player is on; for a leave, the one it left from.
    std::string backend;
    std::string uuid;
    /// The backend a switch came from; for a chat, the message.
    std::string from;
    /// The client's address and port, set on a join.
    std::string address{};
    std::uint16_t port = 0;
    std::int64_t value = 0;
};

/// Title, subtitle and action bar in one go; unset parts are left alone.
struct HudTitle {
    std::optional<wire::text::Component> title;
    std::optional<wire::text::Component> subtitle;
    std::optional<wire::text::Component> actionBar;
    /// Fade in, stay and fade out in ticks.
    std::optional<std::array<std::int32_t, 3>> times;
    /// Clear what is shown, resetting the times too when `reset` is set.
    bool clear = false;
    bool reset = false;
};

/// Header and footer of the tab list; the part left unset keeps what the player already has.
struct HudTabList {
    std::optional<wire::text::Component> header;
    std::optional<wire::text::Component> footer;
};

/// One operation on a boss bar a plugin owns, named by its id.
struct HudBossBar {
    enum class Op : std::uint8_t { kAdd, kRemove, kProgress, kName, kStyle };
    Op op = Op::kAdd;
    std::string id;
    wire::text::Component name;
    float progress = 1.0F;
    /// The bar colour, 0 pink to 6 white, and the overlay, 0 plain to 4 notched in 20.
    std::int32_t color = 5;
    std::int32_t overlay = 0;
};

/// Tells the client to reconnect to another address, keeping its cookies.
struct HudTransfer {
    std::string host;
    std::int32_t port = 25565;
};

/// Hands the client a cookie to keep for the next connection.
struct HudStoreCookie {
    std::string key;
    std::vector<std::uint8_t> data;
};

using PlayerHud = std::variant<HudTitle, HudTabList, HudBossBar, HudTransfer, HudStoreCookie>;

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

enum class DecisionKind : std::uint8_t { kLogin, kPreConnect, kKick, kChat, kCommand, kPing };

/// A question the proxy asks plugins and waits for without blocking an I/O owner; the answer is late after deadlineMs.
struct DecisionRequest {
    std::uint64_t token = 0;
    DecisionKind kind = DecisionKind::kLogin;
    std::string player;
    std::string uuid;
    std::string address;
    std::uint16_t port = 0;
    std::string host;
    /// The backend about to be connected; empty for a login.
    std::string backend;
    /// The backend the player is on now; empty on the first connection.
    std::string from;
    /// The kick reason as plain text, the chat message, the command without its slash, or the ping's protocol.
    std::string text;
    /// A chat message or command that carries a signature: a plugin may neither cancel nor change it.
    bool signedContent = false;
    std::uint32_t deadlineMs = 0;
};

struct DecisionResult {
    bool deny = false;
    /// MiniMessage shown to the player when denied.
    std::string reason;
    /// A backend to use instead of the requested one; empty keeps it.
    std::string redirect;
    /// A replacement for the kick reason (MiniMessage) or the command, or the ping's description (MiniMessage).
    std::string text;
    std::optional<std::int32_t> maxPlayers;
    std::optional<std::int32_t> online;
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
    /// Told name and uuid once a player is authenticated; a returned MiniMessage turns them away with that text.
    std::function<std::optional<std::string>(std::string_view player, std::string_view uuid, std::string_view address)>
        playerLogin = {};
    /// The text of one of the proxy's own refusals by its messages.toml key; empty keeps the built-in English one.
    std::function<std::string(std::string_view key)> kickText = {};
    /// Whether a player is in the operator group; operators are exempt from the spam kick.
    std::function<bool(std::string_view player)> isOperator = {};
    /// Those commands for one player, added to every command tree the proxy relays so clients know them.
    std::function<std::vector<PlayerCommandSpec>(std::string_view player)> playerCommandSpecs = {};
    /// Completions for the last word of a command line a player is typing, when it starts with a proxy command.
    std::function<std::optional<std::vector<std::string>>(std::string_view player, std::string_view line)>
        commandSuggestions = {};
    /// The Mojang keys that sign profile keys, when chat is enforced online; unset fetches them over HTTPS.
    std::function<std::shared_ptr<const wire::auth::ServicesKeySet>()> servicesKeys = {};
    /// MiniMessage text a login is turned away with while the proxy is still starting; empty once it is ready.
    std::function<std::optional<std::string>()> loadingMessage = {};
    /// The tab list, boss bar and scoreboard the proxy draws; unset draws nothing. Runs on owner threads.
    std::function<std::shared_ptr<const wire::config::UiConfigs>()> uiConfigs = {};
    const wire::common::placeholder::PlaceholderRegistry* placeholders = nullptr;
    /// The server list answer for one ping; the proxy's own one-line answer when unset. Runs on owner threads.
    std::function<wire::protocol::status::ServerStatusPayload(std::int32_t protocol, std::string_view host,
                                                              std::string_view address)>
        status = {};
    /// Plugin listeners for relayed packets; unset costs nothing. Runs on owner threads and may block briefly.
    std::shared_ptr<wire::plugins::PluginPacketBus> packets = {};
    /// Packets larger than this go past plugin listeners untouched.
    std::size_t packetMaxBytes = 256 * 1024;
    /// Offered every plugin message in play; true consumes it. Owner threads, must not block.
    std::function<bool(std::string_view player, std::string_view channel, std::span<const std::uint8_t> data,
                       bool fromBackend)>
        pluginMessage = {};
    /// Asks plugins to decide; false means nobody listens and the proxy goes on at once. Owner threads, no blocking.
    std::function<bool(DecisionRequest)> decide = {};
    /// Told a player's round trip to their client in milliseconds, measured on each keepalive. Owner threads.
    std::function<void(std::string_view player, std::int32_t milliseconds)> latency = {};
};

/// One session as its owner sees it; name and uuid stay empty until the player is authenticated.
struct SessionInfo {
    std::string name;
    std::string uuid;
    std::string backend;
    std::int32_t protocol = 0;
    /// handshake, login, configuration or play.
    std::string phase;
};

struct BackendInfo {
    std::string name;
    std::string host;
    std::uint16_t port = 0;
    std::int32_t protocol = 0;
    bool automatic = false;
    std::string forwarding;
    /// False while the health check has the server down; always true with the check off.
    bool healthy = true;
};

struct ServerGroupInfo {
    struct Member {
        std::string name;
        bool healthy = true;
    };

    std::string name;
    std::vector<Member> servers;
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
    /// Thread-safe. The players who have finished joining, in the order they did.
    [[nodiscard]] std::vector<std::string> playerNames() const;
    [[nodiscard]] std::vector<BackendInfo> backends() const;
    [[nodiscard]] std::vector<ServerGroupInfo> serverGroups() const;
    /// Thread-safe, never blocks. The cached status of the server a server list profile's `source` names, if any.
    [[nodiscard]] std::optional<wire::protocol::status::ServerStatusPayload> relayStatus(std::string_view source,
                                                                                         std::string_view host) const;
    /// Thread-safe. Asks every session owner for its sessions and waits up to `timeout` for all of them.
    [[nodiscard]] std::expected<std::vector<SessionInfo>, std::string>
    sessions(std::chrono::milliseconds timeout) const;
    /// Thread-safe. The host and port a backend is configured with.
    [[nodiscard]] std::optional<std::pair<std::string, std::uint16_t>> backendEndpoint(std::string_view name) const;
    /// Thread-safe. Queues a chat line for a player; true once handed to the session owners.
    [[nodiscard]] bool tellPlayer(std::string_view player, std::string_view text);
    /// Thread-safe. Disconnects a player in play with a MiniMessage reason; true when handed to the session owners.
    [[nodiscard]] bool kickPlayer(std::string_view player, std::string_view reason);
    /// Thread-safe. Hands a packet to the session owners toward the client or the backend; true once handed over.
    [[nodiscard]] bool sendPacket(std::string_view player, bool serverbound, std::int32_t phase, std::int32_t id,
                                  std::span<const std::uint8_t> body);
    /// Thread-safe. The same as tellPlayer and kickPlayer with a finished component, no MiniMessage in between.
    [[nodiscard]] bool tellComponent(std::string_view player, wire::text::Component component);
    [[nodiscard]] bool kickComponent(std::string_view player, wire::text::Component component);
    /// Thread-safe. Draws something on a player's screen; true once handed to the session owners.
    [[nodiscard]] bool sendHud(std::string_view player, PlayerHud action);
    /// Thread-safe. Asks the client for a cookie; the reply runs on a session owner thread, empty when it has none.
    [[nodiscard]] bool requestCookie(std::string_view player, std::string_view key,
                                     std::function<void(std::optional<std::vector<std::uint8_t>>)> reply);
    /// Thread-safe. Queues a plugin message toward the player's client or backend; true once handed to the owners.
    [[nodiscard]] bool sendPluginMessage(std::string_view player, std::string_view channel,
                                         std::span<const std::uint8_t> data, bool towardBackend);
    /// Thread-safe. Answers a DecisionRequest; a stale token is ignored.
    void resolveDecision(std::string_view player, std::uint64_t token, DecisionResult result);
    /// Thread-safe. Runs an operator antibot command (status, attack, ban, unban, verify, unverify, verified).
    [[nodiscard]] std::string antibotCommand(std::span<const std::string_view> words);
    /// Thread-safe. Sends a MiniMessage line to every configured operator who is online.
    void notifyOperators(std::string_view text);
    void antibotVerify(std::string_view player);
    void antibotUnverify(std::string_view player);
    [[nodiscard]] bool antibotVerified(std::string_view player) const;
    [[nodiscard]] bool antibotAttack() const;
    /// Thread-safe. Gates a Bedrock server-list ping (connecting false) or a new session (true) by address.
    [[nodiscard]] bool bedrockOffline(std::string_view address, bool connecting);
    /// Thread-safe. Why a Bedrock login is refused, or nothing to let it in.
    [[nodiscard]] std::optional<std::string> bedrockLogin(std::string_view address, std::string_view name,
                                                          std::string_view xuid, bool authenticated);
    void bedrockFailed(std::string_view address);
    [[nodiscard]] int bedrockPacketsPerSecond() const;
    [[nodiscard]] int bedrockLoginSeconds() const;
    /// Milliseconds a server-list answer may be reused right now, 0 when it may not.
    [[nodiscard]] int statusCacheMs() const;
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
