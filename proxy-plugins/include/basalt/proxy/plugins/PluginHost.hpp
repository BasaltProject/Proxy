#pragma once

#include <basalt/proxy/plugins/ForeignAdapter.hpp>
#include <basalt/proxy/runtime/CommandDispatcher.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/lua/LuaArgs.hpp>
#include <basalt/wire/lua/LuaRef.hpp>
#include <basalt/wire/lua/LuaSandbox.hpp>
#include <basalt/wire/plugins/PacketRegistry.hpp>
#include <basalt/wire/plugins/PluginPackage.hpp>
#include <basalt/wire/sql/Sqlite.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace basalt::proxy::plugins {

struct ProxyInfo {
    std::string bind;
    std::uint16_t port = 0;
    bool onlineMode = false;
    bool preventClientProxyConnections = false;
    std::int32_t compressionThreshold = 0;
    std::uint32_t connectTimeoutMs = 0;
    std::uint32_t readTimeoutMs = 0;
    std::size_t maxSessions = 0;
    std::vector<std::string> defaultRoute;
    /// Host name to the backends it routes to.
    std::vector<std::pair<std::string, std::vector<std::string>>> forcedHosts;
};

struct PluginHooks;

/// Builds a foreign adapter from the hooks; null when the operator has nothing for it to load.
using AdapterFactory = std::function<std::unique_ptr<ForeignAdapter>(const PluginHooks&)>;

struct PluginSettings {
    std::string directory = "plugins";
    std::string database;
    wire::lua::LuaLimits limits;
    std::size_t queueSize = 256;
    /// Plugin id to permission names (commands, network, packets) the operator grants.
    std::map<std::string, std::vector<std::string>> allow;
    /// Where packet listeners register; the runtime delivers relayed packets to it. Unset disables the packets API.
    std::shared_ptr<wire::plugins::PacketRegistry> packets;
    /// How long a relayed packet waits for the plugin interpreter before it goes on untouched.
    std::chrono::milliseconds packetTimeout{25};
    /// The most bytes a listener may set as a body or send.
    std::size_t packetMaxBytes = 256 * 1024;
    std::vector<AdapterFactory> adapters;
};

/// What a plugin may reach. Every callback runs on the plugin thread and must not block on session owners.
struct PluginHooks {
    std::function<std::vector<std::string>()> servers;
    /// The operator settings of the proxy as read from its configuration files.
    std::function<ProxyInfo()> info;
    std::function<bool(std::string_view player, std::string_view backend)> send;
    std::function<void(std::string_view plugin, std::string_view line)> log;
    /// Sends a chat line, written in MiniMessage, to a player's client.
    std::function<void(std::string_view player, std::string_view text)> tell;
    /// Disconnects a player in play; the reason is MiniMessage.
    std::function<void(std::string_view player, std::string_view reason)> kick;
    /// Runs a console line as the operator would; true when the line named a command.
    std::function<bool(std::string_view line)> console;
    /// Where plugin commands are registered, so players and the console reach them like the proxy's own.
    runtime::CommandDispatcher* commands = nullptr;
    /// Whether a player's group grants a permission node.
    std::function<bool(std::string_view player, std::string_view node)> hasPermission;
    std::function<std::string(std::string_view player)> groupOf;
    /// False when the player has never joined or the group does not exist; an empty group clears it.
    std::function<bool(std::string_view player, std::string_view group)> setGroup;
    /// Overrides one node for the player until the proxy stops; a negative value removes the override.
    std::function<void(std::string_view player, std::string_view node, int value)> setPermission;
    /// Sends a packet toward a player's client, or its backend as if the client sent it; true once handed over.
    std::function<bool(std::string_view player, bool serverbound, std::int32_t phase, std::int32_t id,
                       std::span<const std::uint8_t> body)>
        packet;
    /// Sends a plugin message toward the player's backend or, when false, the player's client.
    std::function<bool(std::string_view player, std::string_view channel, std::span<const std::uint8_t> data,
                       bool towardBackend)>
        sendMessage;
    std::function<std::optional<std::pair<std::string, std::uint16_t>>(std::string_view server)> serverAddress;
    /// The same as tell and kick with a finished component.
    std::function<void(std::string_view player, wire::text::Component)> tellComponent;
    /// Draws something on a player's screen (title, tab list, boss bar), asks them to transfer, or stores a cookie.
    std::function<bool(std::string_view player, runtime::PlayerHud)> hud;
    /// Asks the client for a cookie; the reply may run on any thread and is empty when the client has none.
    std::function<bool(std::string_view player, std::string_view key,
                       std::function<void(std::optional<std::vector<std::uint8_t>>)>)>
        requestCookie;
    std::function<void(std::string_view player, wire::text::Component)> kickComponent;
    /// Answers a decision once the handlers ran; may be called after the deadline, when the runtime ignores it.
    std::function<void(std::string_view player, std::uint64_t token, runtime::DecisionResult)> resolveDecision;
    std::function<void(std::string_view player)> antibotVerify;
    std::function<void(std::string_view player)> antibotUnverify;
    std::function<bool(std::string_view player)> antibotVerified;
    std::function<bool()> antibotAttack;
    std::function<bool(std::string_view address, bool ban)> antibotBan;
};

/// One thread runs every plugin's Lua, so no Lua state is entered twice and no I/O owner waits on a plugin.
class PluginHost {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<PluginHost>, std::string> start(PluginSettings settings,
                                                                                       PluginHooks hooks);
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    /// Never blocks: a full queue drops the event and counts it.
    void post(runtime::PlayerEvent event);

    /// True when the first word names a command a plugin registered, which is then queued for the plugin thread.
    [[nodiscard]] bool command(std::string_view line);

    /// The same for a command typed by a player; the call is queued for the plugin thread.
    [[nodiscard]] bool playerCommand(std::string_view player, std::string_view line);

    /// Names of the loaded plugins.
    [[nodiscard]] std::vector<std::string> names();

    /// Starts a decision on the plugin thread; false when no plugin decides it, so the caller goes on at once.
    [[nodiscard]] bool decide(runtime::DecisionRequest request);

    /// Offered a plugin message in play; true when a plugin registered the channel or it is the BungeeCord one.
    [[nodiscard]] bool message(std::string_view player, std::string_view channel, std::span<const std::uint8_t> data,
                               bool fromBackend);

    /// Drops every plugin, reads the packages again and resumes; queued events and per-plugin data are kept.
    [[nodiscard]] std::size_t reload();

    [[nodiscard]] std::size_t loaded() const noexcept { return loaded_.load(std::memory_order_relaxed); }

    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    /// Runs one Lua packet listener on the calling thread, a session owner; false when it timed out waiting to run.
    [[nodiscard]] bool deliverPacket(const void* owner, const wire::lua::LuaRef& function,
                                     wire::plugins::PluginPacket& packet);

    /// Relayed packets that went on untouched because the plugin interpreter was busy for longer than the timeout.
    [[nodiscard]] std::size_t packetTimeouts() const noexcept {
        return packetTimeouts_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t delivered() const noexcept { return delivered_.load(std::memory_order_relaxed); }

private:
    friend struct PluginApi;
    struct Plugin;
    struct Entry;
    struct Timer;

    PluginHost(PluginSettings settings, PluginHooks hooks);

    void loadPackages();
    void load(const std::shared_ptr<wire::plugins::PluginPackage>& package,
              const wire::plugins::PluginManifest& manifest);
    void run(std::stop_token stop);
    void shutdown();
    void dropPacketListeners();
    void listPlugins();

    struct ConsoleCall {
        std::vector<std::string> words;
    };

    struct PlayerCommandCall {
        std::string player;
        std::vector<std::string> words;
    };

    struct MessageCall {
        std::string player;
        std::string channel;
        std::vector<std::uint8_t> data;
        bool fromBackend = false;
    };

    struct DecisionCall {
        runtime::DecisionRequest request;
        std::chrono::steady_clock::time_point received;
    };

    struct DecisionRun;

    using Task = std::variant<runtime::PlayerEvent, ConsoleCall, PlayerCommandCall, MessageCall, DecisionCall>;

    void deliverDecision(const DecisionCall& call);
    void stepDecision(const std::shared_ptr<DecisionRun>& run, std::size_t index);

    struct Player {
        std::string name;
        std::string uuid;
        std::string server;
        std::string address{};
        std::uint16_t port = 0;
        std::int64_t ping = -1;
    };

    void deliverMessage(const MessageCall& call);
    [[nodiscard]] std::vector<std::string> playersOn(std::string_view server) const;

    void deliver(const runtime::PlayerEvent& event);
    void invoke(const std::vector<std::string>& words, const std::optional<Player>& source);
    void callLogin(Plugin& plugin, const std::string& player, const std::string& uuid);
    void callAntibot(Plugin& plugin, const runtime::PlayerEvent& event);
    void callPing(Plugin& plugin, const std::string& host, const std::string& protocol);
    void callEvent(Plugin& plugin, const std::string& name, const Player* subject, const std::string& from);
    void call(Plugin& plugin, const wire::lua::LuaRef& function,
              const std::function<std::int32_t(wire::lua::LuaArgs&)>& pushArguments, std::string_view why,
              std::function<void(bool)> after = {});
    void report(Plugin& plugin, const wire::lua::LuaFailure& failure, std::string_view why);
    void runTimers(std::chrono::steady_clock::time_point now);
    [[nodiscard]] bool enqueue(Task task);
    [[nodiscard]] bool allowed(const Player& player, const std::optional<std::string>& permission) const;
    [[nodiscard]] const Player* findPlayer(std::string_view name) const;

    PluginSettings settings_;
    PluginHooks hooks_;
    std::map<std::string, std::pair<std::shared_ptr<wire::plugins::PluginPackage>, wire::plugins::PluginManifest>>
        installed_;
    std::optional<wire::sql::Database> database_;
    std::vector<std::unique_ptr<Plugin>> plugins_;
    std::vector<std::unique_ptr<ForeignAdapter>> adapters_;
    std::set<std::string, std::less<>> foreignCommands_;
    std::atomic<std::size_t> loaded_{0};
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Task> queue_;
    mutable std::shared_mutex commandsMutex_;
    std::mutex reloadMutex_;
    std::vector<std::string> foreignNames_;
    /// Held while any Lua of any plugin runs, on the plugin thread or on a session owner delivering a packet.
    std::recursive_timed_mutex luaMutex_;
    std::atomic<std::size_t> packetTimeouts_{0};
    mutable std::shared_mutex channelsMutex_;
    std::set<std::string, std::less<>> channels_;
    std::map<std::string, std::vector<std::unique_ptr<Entry>>, std::less<>> commands_;
    std::vector<std::unique_ptr<Timer>> timers_;
    std::map<std::string, Player, std::less<>> roster_;
    bool loading_ = true;
    std::atomic<unsigned> deciders_{0};
    std::atomic<std::size_t> dropped_{0};
    std::atomic<std::size_t> delivered_{0};
    std::jthread thread_;
};

} // namespace basalt::proxy::plugins
