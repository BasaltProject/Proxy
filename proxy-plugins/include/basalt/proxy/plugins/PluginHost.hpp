#pragma once

#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/lua/LuaSandbox.hpp>
#include <basalt/wire/plugins/PluginPackage.hpp>
#include <basalt/wire/sql/Sqlite.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
};

/// What a plugin may reach. Every callback runs on the plugin thread and must not block on session owners.
struct PluginHooks {
    std::function<std::vector<std::string>()> servers;
    std::function<bool(std::string_view player, std::string_view backend)> send;
    std::function<void(std::string_view plugin, std::string_view line)> log;
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

    /// True when the first word names a plugin command, which is then queued for the plugin thread; otherwise false.
    [[nodiscard]] bool command(std::string_view line);

    /// Stops the plugin thread, drops every plugin and its commands, reads the packages again and resumes.
    /// Events queued meanwhile are kept; per-plugin data stays in the database. Serialised with itself.
    [[nodiscard]] std::size_t reload();

    [[nodiscard]] std::size_t loaded() const noexcept { return loaded_.load(std::memory_order_relaxed); }

    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    [[nodiscard]] std::size_t delivered() const noexcept { return delivered_.load(std::memory_order_relaxed); }

private:
    struct Plugin;
    struct Access;

    PluginHost(PluginSettings settings, PluginHooks hooks);

    void loadPackages();
    void load(const std::shared_ptr<wire::plugins::PluginPackage>& package,
              const wire::plugins::PluginManifest& manifest);
    void run(std::stop_token stop);

    struct CommandCall {
        std::string name;
        std::vector<std::string> arguments;
    };

    using Task = std::variant<runtime::PlayerEvent, CommandCall>;
    struct Command;

    void deliver(const runtime::PlayerEvent& event);
    void invoke(const CommandCall& call);
    [[nodiscard]] bool enqueue(Task task);

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
    std::map<std::string, std::unique_ptr<Command>, std::less<>> commands_;
    bool loading_ = true;
    std::atomic<std::size_t> dropped_{0};
    std::atomic<std::size_t> delivered_{0};
    std::jthread thread_;
};

} // namespace basalt::proxy::plugins
