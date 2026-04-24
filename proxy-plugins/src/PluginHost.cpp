#include <basalt/proxy/plugins/PluginHost.hpp>
#include <basalt/wire/lua/LuaArgs.hpp>
#include <basalt/wire/lua/LuaRef.hpp>
#include <basalt/wire/plugins/PluginOrder.hpp>
#include <basalt/wire/sql/Migrations.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

namespace basalt::proxy::plugins {
namespace {

constexpr std::size_t kMaxSourceBytes = 1024 * 1024;
constexpr std::array<std::pair<runtime::PlayerEventKind, std::string_view>, 3> kEventNames = {{
    {runtime::PlayerEventKind::kJoin, "join"},
    {runtime::PlayerEventKind::kLeave, "leave"},
    {runtime::PlayerEventKind::kSwitch, "switch"},
}};

} // namespace

struct PluginHost::Plugin {
    std::string name;
    std::unique_ptr<wire::lua::LuaSandbox> sandbox;
    std::map<runtime::PlayerEventKind, std::vector<wire::lua::LuaRef>> handlers;
    bool broken = false;
    PluginHost* host = nullptr;
};

struct PluginHost::Command {
    Plugin* plugin;
    wire::lua::LuaRef handler;
};

struct PluginHost::Access {
    static Plugin* plugin(lua_State* state) { return static_cast<Plugin*>(lua_touserdata(state, lua_upvalueindex(1))); }

    static int log(lua_State* state) {
        Plugin* self = plugin(state);
        std::size_t length = 0;
        const char* text = luaL_checklstring(state, 1, &length);
        if (self->host->hooks_.log)
            self->host->hooks_.log(self->name, std::string_view{text, std::min<std::size_t>(length, 4096)});
        return 0;
    }

    static int on(lua_State* state) {
        Plugin* self = plugin(state);
        const char* name = luaL_checkstring(state, 1);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        for (const auto& [kind, label] : kEventNames) {
            if (label == name) {
                lua_pushvalue(state, 2);
                self->handlers[kind].push_back(wire::lua::LuaRef::take(state));
                return 0;
            }
        }
        return luaL_error(state, "unknown event '%s'", name);
    }

    static int command(lua_State* state) {
        Plugin* self = plugin(state);
        std::size_t length = 0;
        const char* name = luaL_checklstring(state, 1, &length);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        const std::string_view label{name, length};
        if (!self->host->loading_)
            return luaL_error(state, "commands can only be registered while the plugin loads");
        const bool valid = !label.empty() && label.size() <= 32 && std::ranges::all_of(label, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
        if (!valid)
            return luaL_error(state, "command names are 1..32 characters of a-z, 0-9, '_' and '-'");
        constexpr std::array<std::string_view, 6> kBuiltin = {"help", "servers", "status", "reload", "send", "stop"};
        bool refused = std::ranges::find(kBuiltin, label) != kBuiltin.end();
        if (!refused) {
            std::unique_lock lock{self->host->commandsMutex_};
            refused = self->host->commands_.contains(label);
            if (!refused) {
                lua_pushvalue(state, 2);
                self->host->commands_.emplace(std::string{label},
                                              std::make_unique<Command>(Command{self, wire::lua::LuaRef::take(state)}));
            }
        }
        if (refused)
            return luaL_error(state, "command '%s' already exists", name);
        return 0;
    }

    static int servers(lua_State* state) {
        Plugin* self = plugin(state);
        lua_newtable(state);
        if (!self->host->hooks_.servers)
            return 1;
        lua_Integer position = 1;
        for (const std::string& name : self->host->hooks_.servers()) {
            lua_pushlstring(state, name.data(), name.size());
            lua_rawseti(state, -2, position++);
        }
        return 1;
    }

    static constexpr std::size_t kMaxKey = 128;
    static constexpr std::size_t kMaxValue = 64 * 1024;
    static constexpr std::int64_t kMaxKeys = 1000;

    static int dataGet(lua_State* state) {
        Plugin* self = plugin(state);
        std::size_t length = 0;
        const char* key = luaL_checklstring(state, 1, &length);
        if (!self->host->database_ || length > kMaxKey) {
            lua_pushnil(state);
            return 1;
        }
        std::optional<std::string> value;
        {
            auto query = self->host->database_->prepare("SELECT value FROM plugin_data WHERE plugin = ?1 AND key = ?2");
            query.bind(1, self->name);
            query.bind(2, std::string_view{key, length});
            if (query.step())
                value = query.text(0);
        }
        if (value)
            lua_pushlstring(state, value->data(), value->size());
        else
            lua_pushnil(state);
        return 1;
    }

    static int dataSet(lua_State* state) {
        Plugin* self = plugin(state);
        std::size_t keyLength = 0;
        std::size_t valueLength = 0;
        const char* key = luaL_checklstring(state, 1, &keyLength);
        const char* value = luaL_checklstring(state, 2, &valueLength);
        if (!self->host->database_)
            return luaL_error(state, "plugin data is not available");
        if (keyLength == 0 || keyLength > kMaxKey || valueLength > kMaxValue)
            return luaL_error(state, "data key must be 1..128 bytes and value at most 65536 bytes");
        bool stored = false;
        bool full = false;
        {
            auto& database = *self->host->database_;
            auto count = database.prepare("SELECT COUNT(*) FROM plugin_data WHERE plugin = ?1 AND key <> ?2");
            count.bind(1, self->name);
            count.bind(2, std::string_view{key, keyLength});
            full = count.step() && count.integer(0) >= kMaxKeys;
            if (!full) {
                auto write = database.prepare("INSERT INTO plugin_data(plugin, key, value) VALUES(?1, ?2, ?3) "
                                              "ON CONFLICT(plugin, key) DO UPDATE SET value = excluded.value");
                write.bind(1, self->name);
                write.bind(2, std::string_view{key, keyLength});
                write.bind(3, std::string_view{value, valueLength});
                stored = write.execute();
            }
        }
        if (full)
            return luaL_error(state, "plugin data is limited to 1000 keys");
        lua_pushboolean(state, stored ? 1 : 0);
        return 1;
    }

    static int dataDelete(lua_State* state) {
        Plugin* self = plugin(state);
        std::size_t length = 0;
        const char* key = luaL_checklstring(state, 1, &length);
        bool removed = false;
        if (self->host->database_ && length <= kMaxKey) {
            auto remove = self->host->database_->prepare("DELETE FROM plugin_data WHERE plugin = ?1 AND key = ?2");
            remove.bind(1, self->name);
            remove.bind(2, std::string_view{key, length});
            removed = remove.execute();
        }
        lua_pushboolean(state, removed ? 1 : 0);
        return 1;
    }

    static int send(lua_State* state) {
        Plugin* self = plugin(state);
        std::size_t playerLength = 0;
        std::size_t backendLength = 0;
        const char* player = luaL_checklstring(state, 1, &playerLength);
        const char* backend = luaL_checklstring(state, 2, &backendLength);
        const bool admitted =
            self->host->hooks_.send &&
            self->host->hooks_.send(std::string_view{player, playerLength}, std::string_view{backend, backendLength});
        lua_pushboolean(state, admitted ? 1 : 0);
        return 1;
    }
};

PluginHost::PluginHost(PluginSettings settings, PluginHooks hooks)
    : settings_(std::move(settings)), hooks_(std::move(hooks)) {}

PluginHost::~PluginHost() {
    thread_.request_stop();
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

std::expected<std::unique_ptr<PluginHost>, std::string> PluginHost::start(PluginSettings settings, PluginHooks hooks) {
    std::unique_ptr<PluginHost> host{new PluginHost{std::move(settings), std::move(hooks)}};
    std::error_code error;
    if (!host->settings_.database.empty()) {
        const std::filesystem::path file{host->settings_.database};
        if (file.has_parent_path())
            std::filesystem::create_directories(file.parent_path(), error);
        host->database_ = wire::sql::Database::open(host->settings_.database);
        if (!host->database_)
            return std::unexpected("could not open the plugin database " + host->settings_.database);
        constexpr std::array<wire::sql::Migration, 1> kMigrations = {
            {{1, "CREATE TABLE plugin_data(plugin TEXT NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL, "
                 "PRIMARY KEY(plugin, key)) WITHOUT ROWID;"}}};
        if (const auto migrated = wire::sql::migrate(*host->database_, kMigrations); !migrated)
            return std::unexpected("plugin database " + host->settings_.database + ": " + migrated.error());
    }
    host->loadPackages();
    host->loading_ = false;
    host->thread_ = std::jthread{[raw = host.get()](std::stop_token stop) { raw->run(stop); }};
    return host;
}

void PluginHost::loadPackages() {
    auto discovered = wire::plugins::discoverPackages(settings_.directory, ".bppkg");
    if (!discovered) {
        if (hooks_.log)
            hooks_.log("plugins", "could not read the plugin directory: " + discovered.error().message);
        return;
    }
    const auto refuse = [this](std::string_view what, std::string_view why) {
        if (hooks_.log)
            hooks_.log(what, "did not load: " + std::string{why});
    };
    std::vector<std::pair<std::shared_ptr<wire::plugins::PluginPackage>, wire::plugins::PluginManifest>> candidates;
    for (auto& owned : *discovered) {
        const std::shared_ptr<wire::plugins::PluginPackage> package{std::move(owned)};
        const auto text = package->readText(wire::plugins::packageLayout::kManifest);
        if (!text) {
            refuse(package->origin(), "it has no plugin.toml");
            continue;
        }
        auto manifest = wire::plugins::PluginManifest::parse(*text);
        if (!manifest) {
            refuse(package->origin(), manifest.error().message);
            continue;
        }
        if (std::ranges::any_of(candidates, [&](const auto& other) { return other.second.id == manifest->id; })) {
            refuse(package->origin(), "'" + manifest->id + "' is already installed from another package");
            continue;
        }
        candidates.emplace_back(package, std::move(*manifest));
    }
    std::vector<wire::plugins::PluginManifest> manifests;
    for (const auto& [package, manifest] : candidates)
        manifests.push_back(manifest);
    for (const auto& decision : wire::plugins::pluginOrder::resolve(manifests)) {
        if (!decision.loads()) {
            refuse(decision.manifest.id, decision.refusal);
            continue;
        }
        const auto found = std::ranges::find_if(
            candidates, [&](const auto& candidate) { return candidate.second.id == decision.manifest.id; });
        load(found->first, found->second);
    }
}

void PluginHost::load(const std::shared_ptr<wire::plugins::PluginPackage>& package,
                      const wire::plugins::PluginManifest& manifest) {
    const std::string& name = manifest.id;
    const auto report = [this, &name](std::string_view line) {
        if (hooks_.log)
            hooks_.log(name, line);
    };
    auto source = package->read(manifest.main);
    if (!source || source->size() > kMaxSourceBytes) {
        report(manifest.main + " is missing or larger than 1 MiB; plugin skipped");
        return;
    }
    // A plugin sees its own modules first, then those a dependency that loaded before it chose to export.
    std::vector<std::pair<std::shared_ptr<wire::plugins::PluginPackage>, wire::plugins::PluginManifest>> lenders;
    for (const wire::plugins::PluginDependency& dependency : manifest.dependencies)
        if (const auto lender = installed_.find(dependency.id); lender != installed_.end())
            lenders.push_back(lender->second);
    auto plugin = std::make_unique<Plugin>();
    plugin->name = name;
    plugin->host = this;
    plugin->sandbox = wire::lua::LuaSandbox::create(
        name, settings_.limits, 0,
        [package, lenders](std::string_view module) -> std::optional<std::vector<std::uint8_t>> {
            const auto file = wire::plugins::packageLayout::moduleFile(module);
            if (!file)
                return std::nullopt;
            auto bytes = package->read(*file);
            for (std::size_t i = 0; !bytes && i < lenders.size(); ++i)
                if (lenders[i].second.exports.exportsModule(module))
                    bytes = lenders[i].first->read(*file);
            if (bytes && bytes->size() > kMaxSourceBytes)
                return std::nullopt;
            return bytes;
        },
        [report](std::string_view line) { report(line); });
    if (!plugin->sandbox) {
        report("could not create the Lua state; plugin skipped");
        return;
    }
    lua_State* state = plugin->sandbox->state();
    lua_newtable(state);
    const std::array<std::pair<const char*, lua_CFunction>, 5> functions = {{{"log", &Access::log},
                                                                             {"on", &Access::on},
                                                                             {"command", &Access::command},
                                                                             {"servers", &Access::servers},
                                                                             {"send", &Access::send}}};
    for (const auto& [label, function] : functions) {
        lua_pushlightuserdata(state, plugin.get());
        lua_pushcclosure(state, function, 1);
        lua_setfield(state, -2, label);
    }
    lua_newtable(state);
    const std::array<std::pair<const char*, lua_CFunction>, 3> dataFunctions = {
        {{"get", &Access::dataGet}, {"set", &Access::dataSet}, {"delete", &Access::dataDelete}}};
    for (const auto& [label, function] : dataFunctions) {
        lua_pushlightuserdata(state, plugin.get());
        lua_pushcclosure(state, function, 1);
        lua_setfield(state, -2, label);
    }
    lua_setfield(state, -2, "data");
    lua_setglobal(state, "proxy");
    auto chunk =
        plugin->sandbox->compile(std::string_view{reinterpret_cast<const char*>(source->data()), source->size()},
                                 "@" + name + "/" + manifest.main);
    if (!chunk) {
        report("compile failed: " + chunk.error().message);
        return;
    }
    plugin->sandbox->refill();
    if (auto ran = plugin->sandbox->run(*chunk); !ran) {
        report("failed while loading: " + ran.error().message);
        return;
    }
    installed_.emplace(name, std::make_pair(package, manifest));
    plugins_.push_back(std::move(plugin));
    ++loaded_;
}

std::size_t PluginHost::reload() {
    std::lock_guard serial{reloadMutex_};
    thread_.request_stop();
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
    {
        std::unique_lock lock{commandsMutex_};
        commands_.clear();
    }
    plugins_.clear();
    installed_.clear();
    loaded_.store(0, std::memory_order_relaxed);
    loading_ = true;
    loadPackages();
    loading_ = false;
    thread_ = std::jthread{[this](std::stop_token stop) { run(stop); }};
    return loaded();
}

bool PluginHost::enqueue(Task task) {
    {
        std::lock_guard lock{mutex_};
        if (queue_.size() >= settings_.queueSize) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        queue_.push_back(std::move(task));
    }
    wake_.notify_one();
    return true;
}

void PluginHost::post(runtime::PlayerEvent event) { static_cast<void>(enqueue(std::move(event))); }

bool PluginHost::command(std::string_view line) {
    std::vector<std::string> words;
    while (!line.empty()) {
        const auto begin = line.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos)
            break;
        line.remove_prefix(begin);
        const auto end = line.find_first_of(" \t\r\n");
        words.emplace_back(line.substr(0, end));
        if (end == std::string_view::npos || words.size() > 32)
            break;
        line.remove_prefix(end);
    }
    {
        std::shared_lock lock{commandsMutex_};
        if (words.empty() || !commands_.contains(words.front()))
            return false;
    }
    CommandCall call{words.front(), {words.begin() + 1, words.end()}};
    static_cast<void>(enqueue(std::move(call)));
    return true;
}

void PluginHost::run(std::stop_token stop) {
    while (true) {
        Task task;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, stop, [this] { return !queue_.empty(); });
            if (queue_.empty())
                return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        if (const auto* event = std::get_if<runtime::PlayerEvent>(&task))
            deliver(*event);
        else
            invoke(std::get<CommandCall>(task));
        delivered_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PluginHost::invoke(const CommandCall& call) {
    const auto found = commands_.find(call.name);
    if (found == commands_.end() || found->second->plugin->broken)
        return;
    Plugin& plugin = *found->second->plugin;
    lua_State* state = plugin.sandbox->state();
    plugin.sandbox->refill();
    found->second->handler.push();
    lua_newtable(state);
    lua_Integer position = 1;
    for (const std::string& word : call.arguments) {
        lua_pushlstring(state, word.data(), word.size());
        lua_rawseti(state, -2, position++);
    }
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
        const auto failure = plugin.sandbox->failureFromStack();
        if (hooks_.log)
            hooks_.log(plugin.name, "command '" + call.name + "' failed: " + failure.message);
        if (failure.outOfBudget() || failure.outOfMemory())
            plugin.broken = true;
        return;
    }
    std::size_t length = 0;
    if (const char* reply = lua_tolstring(state, -1, &length); reply != nullptr && hooks_.log)
        hooks_.log(plugin.name, std::string_view{reply, std::min<std::size_t>(length, 4096)});
    lua_pop(state, 1);
}

void PluginHost::deliver(const runtime::PlayerEvent& event) {
    for (auto& plugin : plugins_) {
        if (plugin->broken)
            continue;
        const auto found = plugin->handlers.find(event.kind);
        if (found == plugin->handlers.end())
            continue;
        lua_State* state = plugin->sandbox->state();
        for (const wire::lua::LuaRef& handler : found->second) {
            plugin->sandbox->refill();
            handler.push();
            lua_newtable(state);
            lua_pushlstring(state, event.player.data(), event.player.size());
            lua_setfield(state, -2, "player");
            lua_pushlstring(state, event.backend.data(), event.backend.size());
            lua_setfield(state, -2, "backend");
            if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
                const auto failure = plugin->sandbox->failureFromStack();
                if (hooks_.log)
                    hooks_.log(plugin->name, "handler failed: " + failure.message);
                if (failure.outOfBudget() || failure.outOfMemory())
                    plugin->broken = true;
                if (plugin->broken)
                    break;
            }
        }
    }
}

} // namespace basalt::proxy::plugins
