#pragma once

#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/lua/LuaArgs.hpp>
#include <basalt/wire/lua/LuaRef.hpp>
#include <basalt/wire/lua/LuaSandbox.hpp>
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
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace basalt::proxy::plugins {

struct PluginSettings {
    std::string directory = "plugins";
    std::string database;
    wire::lua::LuaLimits limits;
    std::size_t queueSize = 256;
    /// Plugin id to permission names (commands, network, packets) the operator grants.
    std::map<std::string, std::vector<std::string>> allow;
    /// Player names allowed to run a command a plugin guards with a permission.
    std::vector<std::string> operators;
};

/// What a plugin may reach. Every callback runs on the plugin thread and must not block on session owners.
struct PluginHooks {
    std::function<std::vector<std::string>()> servers;
    std::function<bool(std::string_view player, std::string_view backend)> send;
    std::function<void(std::string_view plugin, std::string_view line)> log;
    /// Sends a chat line, written in MiniMessage, to a player's client.
    std::function<void(std::string_view player, std::string_view text)> tell;
    /// Disconnects a player in play; the reason is MiniMessage.
    std::function<void(std::string_view player, std::string_view reason)> kick;
    /// Runs a console line as the operator would; true when the line named a command.
    std::function<bool(std::string_view line)> console;
};

/// One thread runs every plugin's Lua, so a Lua state is never entered twice at once and no I/O owner waits on a
/// plugin.
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
    [[nodiscard]] std::vector<runtime::PlayerCommandSpec> playerCommandSpecs() const;

    /// Stops the plugin thread, drops every plugin and its commands, reads the packages again and resumes.
    /// Events queued meanwhile are kept; per-plugin data stays in the database. Serialised with itself.
    [[nodiscard]] std::size_t reload();

    [[nodiscard]] std::size_t loaded() const noexcept { return loaded_.load(std::memory_order_relaxed); }

    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

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

    struct ConsoleCall {
        std::vector<std::string> words;
    };

    struct PlayerCommandCall {
        std::string player;
        std::vector<std::string> words;
    };

    using Task = std::variant<runtime::PlayerEvent, ConsoleCall, PlayerCommandCall>;

    struct Player {
        std::string name;
        std::string uuid;
        std::string server;
    };

    void deliver(const runtime::PlayerEvent& event);
    void invoke(const std::vector<std::string>& words, const std::optional<Player>& source);
    void callLogin(Plugin& plugin, const std::string& player, const std::string& uuid);
    void callPing(Plugin& plugin, const std::string& host, const std::string& protocol);
    void callEvent(Plugin& plugin, const std::string& name, const Player* subject, const std::string& from);
    void call(Plugin& plugin, const wire::lua::LuaRef& function,
              const std::function<std::int32_t(wire::lua::LuaArgs&)>& pushArguments, std::string_view why);
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
    std::atomic<std::size_t> loaded_{0};
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Task> queue_;
    mutable std::shared_mutex commandsMutex_;
    std::mutex reloadMutex_;
    std::map<std::string, std::vector<std::unique_ptr<Entry>>, std::less<>> commands_;
    std::vector<std::unique_ptr<Timer>> timers_;
    std::map<std::string, Player, std::less<>> roster_;
    bool loading_ = true;
    std::atomic<std::size_t> dropped_{0};
    std::atomic<std::size_t> delivered_{0};
    std::jthread thread_;
};

} // namespace basalt::proxy::plugins
