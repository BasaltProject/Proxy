#pragma once

#include <basalt/proxy/plugins/PluginHost.hpp>
#include <basalt/wire/common/toml/Toml.hpp>
#include <basalt/wire/lua/LuaArgs.hpp>
#include <basalt/wire/lua/LuaCall.hpp>
#include <basalt/wire/lua/LuaRef.hpp>
#include <basalt/wire/plugins/CommandSpec.hpp>

namespace basalt::proxy::plugins {

inline constexpr std::chrono::milliseconds kTick{50};
inline constexpr std::size_t kMaxSourceBytes = 1024 * 1024;

enum class Priority : std::uint8_t { kLowest, kLow, kNormal, kHigh, kHighest, kMonitor };

struct PluginHost::Plugin {
    struct Handler {
        wire::lua::LuaRef function;
        Priority priority = Priority::kNormal;
        std::map<std::string, std::string, std::less<>> filters;
    };

    std::string id;
    std::string name;
    std::string version;
    PluginHost* host = nullptr;
    std::unique_ptr<wire::lua::LuaSandbox> sandbox;
    std::map<std::string, std::vector<Handler>, std::less<>> handlers;
    std::vector<wire::lua::LuaRef> onEnable;
    std::vector<wire::lua::LuaRef> onDisable;
    wire::lua::LuaRef playerMeta;
    wire::lua::LuaRef consoleMeta;
    bool storage = false;
    bool playersData = false;
    int strikes = 0;
    bool broken = false;
};

struct PluginHost::Entry {
    Plugin* plugin = nullptr;
    wire::plugins::CommandSpec spec;
    wire::lua::LuaRef function;
};

struct PluginHost::Timer {
    std::chrono::steady_clock::time_point due;
    std::chrono::milliseconds period{0};
    Plugin* plugin = nullptr;
    std::shared_ptr<wire::lua::LuaRef> function;
    std::function<void()> resume;
    std::shared_ptr<std::atomic<bool>> cancelled;
};

/// Everything a plugin can call, installed as globals in its sandbox.
struct PluginApi {
    static void install(PluginHost& host, PluginHost::Plugin& plugin, const wire::plugins::PluginManifest& manifest,
                        const wire::common::toml::TomlTable& config);
    static void pushPlayer(wire::lua::LuaArgs& lua, PluginHost::Plugin& plugin, const PluginHost::Player& player);
};

[[nodiscard]] std::string_view priorityName(Priority priority);
[[nodiscard]] std::optional<Priority> priorityOf(std::string_view name);

inline constexpr std::string_view kEnableEvent = "plugin.enable";
inline constexpr std::string_view kDisableEvent = "plugin.disable";
inline constexpr std::string_view kJoinEvent = "player.join";
inline constexpr std::string_view kQuitEvent = "player.quit";
inline constexpr std::string_view kServerChangeEvent = "player.server_change";

} // namespace basalt::proxy::plugins
