#include "PluginInternal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>

namespace basalt::proxy::plugins {
namespace {

using wire::lua::LuaArgs;
using wire::lua::LuaRef;

constexpr std::size_t kMaxKey = 128;
constexpr std::size_t kMaxValue = 64 * 1024;
constexpr std::int64_t kMaxKeys = 1000;
constexpr std::size_t kMaxLine = 4096;
constexpr std::array<std::string_view, 7> kBuiltinCommands = {"help", "servers", "status", "reload",
                                                              "send", "stop",    "plugins"};

void pushToml(LuaArgs& lua, const wire::common::toml::TomlTable& table);

void pushTomlValue(LuaArgs& lua, const wire::common::toml::TomlValue& value) {
    if (const auto* text = value.get<std::string>(); text != nullptr) {
        lua.pushString(*text);
    } else if (const auto* whole = value.get<std::int64_t>(); whole != nullptr) {
        lua.pushInteger(*whole);
    } else if (const auto* real = value.get<double>(); real != nullptr) {
        lua.pushNumber(*real);
    } else if (const auto* flag = value.get<bool>(); flag != nullptr) {
        lua.pushBoolean(*flag);
    } else if (const auto* list = value.get<std::vector<wire::common::toml::TomlValue>>(); list != nullptr) {
        lua.pushTable();
        std::int64_t position = 1;
        for (const auto& element : *list) {
            pushTomlValue(lua, element);
            lua.setIndex(position++);
        }
    } else if (const auto* nested = value.get<wire::common::toml::TomlTable>(); nested != nullptr) {
        pushToml(lua, *nested);
    } else {
        lua.pushNil();
    }
}

void pushToml(LuaArgs& lua, const wire::common::toml::TomlTable& table) {
    lua.pushTable();
    for (const std::string& key : table.keys()) {
        const auto* value = table.find(key);
        if (value == nullptr)
            continue;
        pushTomlValue(lua, *value);
        lua.setField(key);
        if (key.find('-') != std::string::npos) {
            std::string underscored = key;
            std::ranges::replace(underscored, '-', '_');
            pushTomlValue(lua, *value);
            lua.setField(underscored);
        }
    }
}

void pushHandle(LuaArgs& lua, std::shared_ptr<std::atomic<bool>> cancelled) {
    lua.pushTable();
    wire::lua::pushHostFunction(lua, "cancel", [cancelled](LuaArgs&) {
        cancelled->store(true);
        return 0;
    });
    lua.setField("cancel");
}

[[nodiscard]] std::optional<std::string> encode(LuaArgs& lua, std::int32_t index) {
    if (lua.isBoolean(index))
        return lua.toBoolean(index) ? "b1" : "b0";
    if (lua.isInteger(index))
        return "n" + std::to_string(lua.toInteger(index));
    if (lua.isNumber(index)) {
        std::array<char, 40> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), lua.toNumber(index));
        return "n" + std::string{buffer.data(), end};
    }
    if (lua.isString(index))
        return "s" + lua.toText(index);
    return std::nullopt;
}

void pushDecoded(LuaArgs& lua, const std::string& stored) {
    if (stored.empty()) {
        lua.pushNil();
    } else if (stored[0] == 's') {
        lua.pushString(std::string_view{stored}.substr(1));
    } else if (stored[0] == 'b') {
        lua.pushBoolean(stored.size() > 1 && stored[1] == '1');
    } else {
        const std::string_view number = std::string_view{stored}.substr(1);
        std::int64_t whole = 0;
        const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), whole);
        if (error == std::errc{} && end == number.data() + number.size()) {
            lua.pushInteger(whole);
        } else {
            lua.pushNumber(std::strtod(std::string{number}.c_str(), nullptr));
        }
    }
}

} // namespace

void PluginApi::pushPlayer(LuaArgs& lua, PluginHost::Plugin& plugin, const PluginHost::Player& player) {
    lua.pushTable();
    lua.pushString(player.name);
    lua.setField("name");
    lua.pushString(player.uuid);
    lua.setField("uuid");
    lua.pushString(player.server);
    lua.setField("server");
    lua.pushBoolean(false);
    lua.setField("is_console");
    // A table that reads and writes this plugin's values for this player in the database.
    PluginHost* host = plugin.host;
    PluginHost::Plugin* self = &plugin;
    const std::string key = player.uuid.empty() ? player.name : player.uuid;
    lua.pushTable();
    lua.pushTable();
    wire::lua::pushHostFunction(lua, "player.data.get", [host, self, key](LuaArgs& args) {
        if (!self->playersData)
            return args.fail("player.data needs players_data = true under [capabilities] in plugin.toml");
        const std::string name = args.checkString(2);
        std::optional<std::string> value;
        if (host->database_ && name.size() <= kMaxKey) {
            auto query = host->database_->prepare("SELECT value FROM player_data WHERE plugin = ?1 AND player = ?2 "
                                                  "AND key = ?3");
            query.bind(1, self->id);
            query.bind(2, key);
            query.bind(3, name);
            if (query.step())
                value = query.text(0);
        }
        if (value)
            pushDecoded(args, *value);
        else
            args.pushNil();
        return 1;
    });
    lua.setField("__index");
    wire::lua::pushHostFunction(lua, "player.data.set", [host, self, key](LuaArgs& args) {
        if (!self->playersData)
            return args.fail("player.data needs players_data = true under [capabilities] in plugin.toml");
        const std::string name = args.checkString(2);
        if (!host->database_)
            return args.fail("plugin data is not available");
        if (name.empty() || name.size() > kMaxKey)
            return args.fail("a data key is 1 to 128 bytes");
        auto& database = *host->database_;
        if (args.isNil(3)) {
            auto remove = database.prepare("DELETE FROM player_data WHERE plugin = ?1 AND player = ?2 AND key = ?3");
            remove.bind(1, self->id);
            remove.bind(2, key);
            remove.bind(3, name);
            static_cast<void>(remove.execute());
            return 0;
        }
        const auto encoded = encode(args, 3);
        if (!encoded)
            return args.fail("only strings, numbers and booleans can be stored");
        const std::string& value = *encoded;
        if (value.size() > kMaxValue)
            return args.fail("a data value is at most 65536 bytes");
        auto count =
            database.prepare("SELECT COUNT(*) FROM player_data WHERE plugin = ?1 AND player = ?2 AND key <> ?3");
        count.bind(1, self->id);
        count.bind(2, key);
        count.bind(3, name);
        if (count.step() && count.integer(0) >= kMaxKeys)
            return args.fail("a player's data is limited to 1000 keys per plugin");
        auto write = database.prepare("INSERT INTO player_data(plugin, player, key, value) VALUES(?1, ?2, ?3, ?4) "
                                      "ON CONFLICT(plugin, player, key) DO UPDATE SET value = excluded.value");
        write.bind(1, self->id);
        write.bind(2, key);
        write.bind(3, name);
        write.bind(4, value);
        static_cast<void>(write.execute());
        return 0;
    });
    lua.setField("__newindex");
    lua.setMetatable(-2);
    lua.setField("data");
    plugin.playerMeta.push(lua);
    lua.setMetatable(-2);
}

void PluginApi::install(PluginHost& host, PluginHost::Plugin& plugin, const wire::plugins::PluginManifest& manifest,
                        const wire::common::toml::TomlTable& config) {
    LuaArgs lua{plugin.sandbox->state()};
    PluginHost* server = &host;
    PluginHost::Plugin* self = &plugin;

    lua.pushTable();
    lua.pushString(plugin.id);
    lua.setField("id");
    lua.pushString(plugin.name);
    lua.setField("name");
    lua.pushString(plugin.version);
    lua.setField("version");
    lua.setGlobal("plugin");

    lua.pushTable();
    for (const std::string_view level : {"info", "warn", "error"}) {
        const std::string prefix = level == "info" ? std::string{} : std::string{level} + ": ";
        wire::lua::pushHostFunction(lua, "log." + std::string{level}, [server, self, prefix](LuaArgs& args) {
            if (server->hooks_.log)
                server->hooks_.log(self->id, prefix + args.toText(1).substr(0, kMaxLine));
            return 0;
        });
        lua.setField(level);
    }
    lua.setGlobal("log");

    pushToml(lua, config);
    lua.setGlobal("config");

    // The metatable every player table shares, and the one the console source has.
    lua.pushTable();
    lua.pushTable();
    wire::lua::pushHostFunction(lua, "send", [server, self](LuaArgs& args) {
        const auto name = args.fieldAsText(1, "name");
        if (!name)
            return args.fail("send needs a player: use player:send(text)");
        if (server->hooks_.tell)
            server->hooks_.tell(*name, self->text->resolve(args.checkString(2), &args, 3).substr(0, kMaxLine));
        return 0;
    });
    lua.setField("send");
    wire::lua::pushHostFunction(lua, "connect", [server](LuaArgs& args) {
        const auto name = args.fieldAsText(1, "name");
        if (!name)
            return args.fail("connect needs a player: use player:connect(server)");
        args.pushBoolean(server->hooks_.send && server->hooks_.send(*name, args.checkString(2)));
        return 1;
    });
    lua.setField("connect");
    wire::lua::pushHostFunction(lua, "kick", [server, self](LuaArgs& args) {
        const auto name = args.fieldAsText(1, "name");
        if (!name)
            return args.fail("kick needs a player: use player:kick(reason)");
        if (server->hooks_.kick)
            server->hooks_.kick(
                *name,
                self->text->resolve(args.stringOrNull(2).value_or("Disconnected"), &args, 3).substr(0, kMaxLine));
        return 0;
    });
    lua.setField("kick");
    lua.setField("__index");
    plugin.playerMeta = LuaRef::take(lua.state());

    lua.pushTable();
    lua.pushTable();
    wire::lua::pushHostFunction(lua, "send", [server, self](LuaArgs& args) {
        if (server->hooks_.log)
            server->hooks_.log(self->id, self->text->resolve(args.checkString(2), &args, 3).substr(0, kMaxLine));
        return 0;
    });
    lua.setField("send");
    lua.setField("__index");
    plugin.consoleMeta = LuaRef::take(lua.state());

    const auto pushOne = [self](LuaArgs& args, const PluginHost::Player* player) {
        if (player == nullptr)
            args.pushNil();
        else
            pushPlayer(args, *self, *player);
    };
    lua.pushTable();
    wire::lua::pushHostFunction(lua, "players.online", [server, self](LuaArgs& args) {
        args.pushTable();
        std::int64_t position = 1;
        for (const auto& [name, player] : server->roster_) {
            pushPlayer(args, *self, player);
            args.setIndex(position++);
        }
        return 1;
    });
    lua.setField("online");
    wire::lua::pushHostFunction(lua, "players.by_name", [server, pushOne](LuaArgs& args) {
        pushOne(args, server->findPlayer(args.checkString(1)));
        return 1;
    });
    lua.setField("by_name");
    wire::lua::pushHostFunction(lua, "players.by_uuid", [server, pushOne](LuaArgs& args) {
        const std::string uuid = args.checkString(1);
        const PluginHost::Player* found = nullptr;
        for (const auto& [name, player] : server->roster_)
            if (player.uuid == uuid)
                found = &player;
        pushOne(args, found);
        return 1;
    });
    lua.setField("by_uuid");
    wire::lua::pushHostFunction(lua, "players.count", [server](LuaArgs& args) {
        args.pushInteger(static_cast<std::int64_t>(server->roster_.size()));
        return 1;
    });
    lua.setField("count");
    lua.setGlobal("players");

    lua.pushTable();
    wire::lua::pushHostFunction(lua, "servers.list", [server](LuaArgs& args) {
        args.pushTable();
        std::int64_t position = 1;
        if (server->hooks_.servers)
            for (const std::string& name : server->hooks_.servers()) {
                args.pushString(name);
                args.setIndex(position++);
            }
        return 1;
    });
    lua.setField("list");
    wire::lua::pushHostFunction(lua, "servers.exists", [server](LuaArgs& args) {
        const std::string wanted = args.checkString(1);
        bool found = false;
        if (server->hooks_.servers) {
            const auto names = server->hooks_.servers();
            found = std::ranges::find(names, wanted) != names.end();
        }
        args.pushBoolean(found);
        return 1;
    });
    lua.setField("exists");
    lua.setGlobal("servers");

    wire::lua::pushHostFunction(lua, "t", [self](LuaArgs& args) {
        args.pushString(self->text->resolve("@" + args.checkString(1), &args, 2));
        return 1;
    });
    lua.setGlobal("t");

    wire::lua::pushHostFunction(lua, "broadcast", [server, self](LuaArgs& args) {
        const std::string text = self->text->resolve(args.checkString(1), &args, 2).substr(0, kMaxLine);
        if (server->hooks_.tell)
            for (const auto& [name, player] : server->roster_)
                server->hooks_.tell(name, text);
        return 0;
    });
    lua.setGlobal("broadcast");

    const bool commands = manifest.capabilities.commands;
    wire::lua::pushHostFunction(lua, "run", [server, self, commands](LuaArgs& args) {
        if (!commands)
            return args.fail(
                "run needs commands = true under [capabilities] in plugin.toml, and the operator to allow it");
        const std::string line = args.checkString(1).substr(0, kMaxLine);
        if (server->hooks_.log)
            server->hooks_.log(self->id, "ran /" + line);
        args.pushBoolean(server->hooks_.console && server->hooks_.console(line));
        return 1;
    });
    lua.setGlobal("run");

    const auto schedule = [server, self](PluginHost::Timer timer) {
        timer.plugin = self;
        server->timers_.push_back(std::make_unique<PluginHost::Timer>(std::move(timer)));
    };
    wire::lua::installWaitingFunction(*plugin.sandbox, "wait",
                                      [schedule, self](LuaArgs& args, wire::lua::LuaCall::Resume resume) {
                                          const auto ticks = std::max<std::int64_t>(1, args.integerOr(1, 1));
                                          PluginHost::Timer timer;
                                          timer.due = std::chrono::steady_clock::now() + kTick * ticks;
                                          timer.resume = std::move(resume);
                                          schedule(std::move(timer));
                                          static_cast<void>(self);
                                      });
    wire::lua::pushHostFunction(lua, "later", [schedule](LuaArgs& args) {
        const auto ticks = std::max<std::int64_t>(0, args.checkInteger(1));
        if (!args.isFunction(2))
            return args.fail("later(ticks, fn) needs a function");
        PluginHost::Timer timer;
        timer.due = std::chrono::steady_clock::now() + kTick * ticks;
        timer.function = std::make_shared<LuaRef>(LuaRef::of(args.state(), 2));
        timer.cancelled = std::make_shared<std::atomic<bool>>(false);
        const auto cancelled = timer.cancelled;
        schedule(std::move(timer));
        pushHandle(args, cancelled);
        return 1;
    });
    lua.setGlobal("later");
    wire::lua::pushHostFunction(lua, "every", [schedule](LuaArgs& args) {
        const bool three = args.count() >= 3;
        const auto delay = std::max<std::int64_t>(0, args.checkInteger(1));
        const auto period = std::max<std::int64_t>(1, three ? args.checkInteger(2) : args.checkInteger(1));
        const std::int32_t at = three ? 3 : 2;
        if (!args.isFunction(at))
            return args.fail("every(period, fn) needs a function");
        PluginHost::Timer timer;
        timer.due = std::chrono::steady_clock::now() + kTick * delay;
        timer.period = kTick * period;
        timer.function = std::make_shared<LuaRef>(LuaRef::of(args.state(), at));
        timer.cancelled = std::make_shared<std::atomic<bool>>(false);
        const auto cancelled = timer.cancelled;
        schedule(std::move(timer));
        pushHandle(args, cancelled);
        return 1;
    });
    lua.setGlobal("every");

    for (const bool observer : {false, true}) {
        const std::string verb = observer ? "watch" : "on";
        wire::lua::pushHostFunction(lua, verb, [self, verb, observer](LuaArgs& args) {
            const std::string name = args.checkString(1);
            const bool hasOptions = args.count() >= 3;
            const std::int32_t options = hasOptions ? 2 : 0;
            const std::int32_t at = hasOptions ? 3 : 2;
            if (!args.isFunction(at))
                return args.fail(verb + "(event, fn) needs a function");
            if (name == kEnableEvent || name == kDisableEvent) {
                if (observer)
                    return args.fail("'" + name + "' is about this plugin: listen to it with on(), not watch()");
                (name == kEnableEvent ? self->onEnable : self->onDisable).push_back(LuaRef::of(args.state(), at));
                return 0;
            }
            if (name != kJoinEvent && name != kQuitEvent && name != kServerChangeEvent && name != kChatEvent &&
                name != kKickEvent && name != kPingEvent && name != kLoginEvent)
                return args.fail("there is no event called '" + name +
                                 "'; this proxy has plugin.enable, plugin.disable, " + std::string{kJoinEvent} + ", " +
                                 std::string{kQuitEvent} + ", " + std::string{kServerChangeEvent} + ", " +
                                 std::string{kChatEvent} + ", " + std::string{kKickEvent} + ", " +
                                 std::string{kPingEvent} + ", " + std::string{kLoginEvent});
            PluginHost::Plugin::Handler handler;
            handler.function = LuaRef::of(args.state(), at);
            if (options != 0 && args.isTable(options)) {
                args.pushField(options, "priority");
                if (args.isString(-1)) {
                    const std::string written = args.toText(-1);
                    args.pop(1);
                    const auto spelled = wire::plugins::priorityOf(written);
                    if (!spelled)
                        return args.fail("'" + written + "' is not a priority");
                    handler.priority = *spelled;
                } else {
                    args.pop(1);
                }
                for (const std::string_view field : {"player_name", "player_uuid", "server", "from_server"}) {
                    args.pushField(options, field);
                    if (args.isString(-1))
                        handler.filters.emplace(std::string{field}, args.toText(-1));
                    args.pop(1);
                }
            }
            auto& list = self->handlers[name];
            const auto position =
                std::ranges::upper_bound(list, handler.priority, {}, &PluginHost::Plugin::Handler::priority);
            list.insert(position, std::move(handler));
            return 0;
        });
        lua.setGlobal(verb);
    }

    wire::lua::pushHostFunction(lua, "cmd", [server, self](LuaArgs& args) {
        const std::string spec = args.checkString(1);
        if (!args.isFunction(2))
            return args.fail("cmd(spec, fn) needs a function");
        std::optional<std::string> permission;
        if (args.isTable(3)) {
            args.pushField(3, "permission");
            if (args.isString(-1))
                permission = args.toText(-1);
            args.pop(1);
        }
        auto parsed = wire::plugins::CommandSpec::parse(spec, std::move(permission));
        if (!parsed)
            return args.fail(parsed.error().message);
        const std::string& root = parsed->name();
        const bool valid = root.size() <= 32 && std::ranges::all_of(root, [](char c) {
                               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
                           });
        if (!valid)
            return args.fail("command names are 1..32 characters of a-z, 0-9, '_' and '-'");
        if (std::ranges::find(kBuiltinCommands, root) != kBuiltinCommands.end())
            return args.fail("'" + root + "' is a command of the proxy itself");
        auto entry = std::make_unique<PluginHost::Entry>();
        entry->plugin = self;
        entry->spec = std::move(*parsed);
        entry->function = LuaRef::of(args.state(), 2);
        std::unique_lock lock{server->commandsMutex_};
        auto& entries = server->commands_[root];
        if (std::ranges::any_of(entries,
                                [&](const auto& other) { return other->spec.literals == entry->spec.literals; }))
            return args.fail("'" + entry->spec.usage() + "' is already registered");
        entries.push_back(std::move(entry));
        return 0;
    });
    lua.setGlobal("cmd");

    const bool storage = manifest.capabilities.storage;
    lua.pushTable();
    const auto problem = [server, storage]() -> const char* {
        if (!storage)
            return "store needs storage = true under [capabilities] in plugin.toml";
        if (!server->database_)
            return "plugin data is not available";
        return nullptr;
    };
    wire::lua::pushHostFunction(lua, "store.get", [self, server, problem](LuaArgs& args) {
        if (const char* why = problem())
            return args.fail(why);
        auto& database = *server->database_;
        const std::string name = args.checkString(1);
        std::optional<std::string> value;
        if (name.size() <= kMaxKey) {
            auto query = database.prepare("SELECT value FROM plugin_data WHERE plugin = ?1 AND key = ?2");
            query.bind(1, self->id);
            query.bind(2, name);
            if (query.step())
                value = query.text(0);
        }
        if (value)
            pushDecoded(args, *value);
        else
            args.pushNil();
        return 1;
    });
    lua.setField("get");
    wire::lua::pushHostFunction(lua, "store.set", [self, server, problem](LuaArgs& args) {
        if (const char* why = problem())
            return args.fail(why);
        auto& database = *server->database_;
        const std::string name = args.checkString(1);
        if (name.empty() || name.size() > kMaxKey)
            return args.fail("a store key is 1 to 128 bytes");
        const auto encoded = encode(args, 2);
        if (!encoded)
            return args.fail("only strings, numbers and booleans can be stored");
        const std::string& value = *encoded;
        if (value.size() > kMaxValue)
            return args.fail("a store value is at most 65536 bytes");
        auto count = database.prepare("SELECT COUNT(*) FROM plugin_data WHERE plugin = ?1 AND key <> ?2");
        count.bind(1, self->id);
        count.bind(2, name);
        if (count.step() && count.integer(0) >= kMaxKeys)
            return args.fail("a plugin's store is limited to 1000 keys");
        auto write = database.prepare("INSERT INTO plugin_data(plugin, key, value) VALUES(?1, ?2, ?3) "
                                      "ON CONFLICT(plugin, key) DO UPDATE SET value = excluded.value");
        write.bind(1, self->id);
        write.bind(2, name);
        write.bind(3, value);
        args.pushBoolean(write.execute());
        return 1;
    });
    lua.setField("set");
    wire::lua::pushHostFunction(lua, "store.delete", [self, server, problem](LuaArgs& args) {
        if (const char* why = problem())
            return args.fail(why);
        auto& database = *server->database_;
        auto remove = database.prepare("DELETE FROM plugin_data WHERE plugin = ?1 AND key = ?2");
        remove.bind(1, self->id);
        remove.bind(2, args.checkString(1));
        args.pushBoolean(remove.execute());
        return 1;
    });
    lua.setField("delete");
    wire::lua::pushHostFunction(lua, "store.keys", [self, server, problem](LuaArgs& args) {
        if (const char* why = problem())
            return args.fail(why);
        auto& database = *server->database_;
        args.pushTable();
        auto query = database.prepare("SELECT key FROM plugin_data WHERE plugin = ?1 ORDER BY key LIMIT 1000");
        query.bind(1, self->id);
        std::int64_t position = 1;
        while (query.step()) {
            args.pushString(query.text(0));
            args.setIndex(position++);
        }
        return 1;
    });
    lua.setField("keys");
    lua.setGlobal("store");
}

} // namespace basalt::proxy::plugins
