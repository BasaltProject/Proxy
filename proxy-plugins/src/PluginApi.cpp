#include "PluginInternal.hpp"

#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/plugins/PacketNaming.hpp>
#include <basalt/wire/plugins/PacketView.hpp>
#include <basalt/wire/plugins/PluginData.hpp>
#include <basalt/wire/plugins/PluginListeners.hpp>
#include <basalt/wire/plugins/PluginScheduling.hpp>
#include <basalt/wire/plugins/PluginToml.hpp>
#include <basalt/wire/plugins/PluginValue.hpp>
#include <basalt/wire/storage/PluginDataRepository.hpp>
#include <basalt/wire/text/ComponentJson.hpp>

#include <algorithm>
#include <span>
#include <utility>

namespace basalt::proxy::plugins {

std::optional<wire::text::Component> componentOfJson(std::string_view json) {
    const auto parsed = wire::common::parseJson(json);
    if (!parsed)
        return std::nullopt;
    return wire::text::componentJson::deserialize(*parsed);
}

namespace {

using wire::lua::LuaArgs;
using wire::lua::LuaRef;

constexpr std::string_view kServerScope = "server";
constexpr std::int64_t kMaxKeys = 1000;
constexpr std::size_t kMaxLine = 4096;
constexpr std::array<std::string_view, 16> kBuiltinCommands = {
    "help", "list", "glist", "server",    "send", "stop",   "reload",  "plugins",
    "op",   "deop", "group", "whitelist", "ban",  "pardon", "banlist", "kick"};

[[nodiscard]] wire::plugins::DataScopeHost dataScope(wire::sql::Database* database, const std::string& pluginId,
                                                      const std::string& scope, bool allowed, const char* denied,
                                                      const char* full, bool ranked) {
    wire::plugins::DataScopeHost host;
    host.problem = [database, allowed, denied]() -> const char* {
        if (!allowed) {
            return denied;
        }
        return database == nullptr ? "plugin data is not available" : nullptr;
    };
    host.get = [database, pluginId, scope](std::string_view key) {
        return wire::storage::PluginDataRepository{*database}.get(pluginId, scope, key);
    };
    host.set = [database, pluginId, scope, full](std::string_view key, const std::optional<std::string>& value) {
        wire::storage::PluginDataRepository repository{*database};
        if (value.has_value()) {
            auto held = repository.all(pluginId, scope);
            if (std::cmp_greater_equal(held.size(), kMaxKeys) && !held.contains(key)) {
                return std::optional<std::string>{full};
            }
        }
        repository.set(pluginId, scope, key, value);
        return std::optional<std::string>{};
    };
    if (ranked) {
        host.top = [database, pluginId](std::string_view key, std::int32_t limit) {
            return wire::storage::PluginDataRepository{*database}.top(pluginId, key, limit);
        };
    }
    return host;
}

runtime::CommandNode nodeOf(const wire::plugins::CommandSpec& spec, runtime::CommandExecutor executor) {
    using Type = wire::plugins::CommandArgumentType;
    std::vector<runtime::CommandNode> chain;
    chain.push_back(runtime::CommandNode::literal(spec.literals.front()));
    for (std::size_t i = 1; i < spec.literals.size(); ++i)
        chain.push_back(runtime::CommandNode::literal(spec.literals[i]));
    for (const auto& argument : spec.arguments) {
        const auto kind = argument.type == Type::Text    ? runtime::ArgumentKind::Text
                          : argument.type == Type::Int   ? runtime::ArgumentKind::Int
                          : argument.type == Type::Float ? runtime::ArgumentKind::Float
                          : argument.type == Type::Bool  ? runtime::ArgumentKind::Bool
                                                         : runtime::ArgumentKind::Word;
        chain.push_back(
            runtime::CommandNode::argument(argument.name, kind, argument.optional).range(argument.min, argument.max));
    }
    runtime::CommandNode node = std::move(chain.back());
    node.executes(std::move(executor));
    chain.pop_back();
    while (!chain.empty()) {
        runtime::CommandNode parent = std::move(chain.back());
        chain.pop_back();
        parent.then(std::move(node));
        node = std::move(parent);
    }
    return node;
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
    lua.pushString(player.address);
    lua.setField("address");
    lua.pushInteger(player.port);
    lua.setField("port");
    lua.pushInteger(player.ping);
    lua.setField("ping");
    lua.pushBoolean(false);
    lua.setField("is_console");
    wire::lua::pushHostFunction(lua, "player.has_permission", [host = plugin.host, name = player.name](LuaArgs& args) {
        const std::string node = args.checkString(2);
        args.pushBoolean(host->hooks_.hasPermission && host->hooks_.hasPermission(name, node));
        return 1;
    });
    lua.setField("has_permission");
    const std::string key = player.uuid.empty() ? player.name : player.uuid;
    wire::plugins::pushDataScope(
        lua, dataScope(plugin.host->database_ ? &*plugin.host->database_ : nullptr, plugin.id, key, plugin.playersData,
                       "player.data needs players_data = true under [capabilities] in plugin.toml",
                       "a player's data is limited to 1000 keys per plugin", false));
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

    wire::plugins::pushToml(lua, config);
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
    wire::lua::pushHostFunction(lua, "send_json", [server](LuaArgs& args) {
        const auto name = args.fieldAsText(1, "name");
        if (!name)
            return args.fail("send_json needs a player: use player:send_json(json)");
        const auto component = componentOfJson(args.checkString(2));
        if (!component)
            return args.fail("send_json needs a text component written as JSON");
        if (server->hooks_.tellComponent)
            server->hooks_.tellComponent(*name, *component);
        return 0;
    });
    lua.setField("send_json");
    wire::lua::pushHostFunction(lua, "kick_json", [server](LuaArgs& args) {
        const auto name = args.fieldAsText(1, "name");
        if (!name)
            return args.fail("kick_json needs a player: use player:kick_json(json)");
        const auto component = componentOfJson(args.checkString(2));
        if (!component)
            return args.fail("kick_json needs a text component written as JSON");
        if (server->hooks_.kickComponent)
            server->hooks_.kickComponent(*name, *component);
        return 0;
    });
    lua.setField("kick_json");
    wire::lua::pushHostFunction(lua, "message", [server](LuaArgs& args) {
        const auto name = args.fieldAsText(1, "name");
        if (!name)
            return args.fail("message needs a player: use player:message(channel, data)");
        const std::string channel = args.checkString(2);
        const std::string data = args.checkString(3);
        const bool toClient = args.stringOrNull(4).value_or("backend") == "client";
        const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(data.data()), data.size()};
        args.pushBoolean(server->hooks_.sendMessage && server->hooks_.sendMessage(*name, channel, bytes, !toClient));
        return 1;
    });
    lua.setField("message");
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
        const auto where = args.stringOrNull(1);
        args.pushInteger(static_cast<std::int64_t>(where ? server->playersOn(*where).size() : server->roster_.size()));
        return 1;
    });
    lua.setField("count");
    lua.setGlobal("players");

    lua.pushTable();
    wire::lua::pushHostFunction(lua, "antibot.is_verified", [server](LuaArgs& args) {
        args.pushBoolean(server->hooks_.antibotVerified && server->hooks_.antibotVerified(args.checkString(1)));
        return 1;
    });
    lua.setField("is_verified");
    wire::lua::pushHostFunction(lua, "antibot.verify", [server](LuaArgs& args) {
        const std::string name = args.checkString(1);
        if (server->hooks_.antibotVerify)
            server->hooks_.antibotVerify(name);
        return 0;
    });
    lua.setField("verify");
    wire::lua::pushHostFunction(lua, "antibot.unverify", [server](LuaArgs& args) {
        const std::string name = args.checkString(1);
        if (server->hooks_.antibotUnverify)
            server->hooks_.antibotUnverify(name);
        return 0;
    });
    lua.setField("unverify");
    wire::lua::pushHostFunction(lua, "antibot.ban", [server](LuaArgs& args) {
        const std::string address = args.checkString(1);
        args.pushBoolean(server->hooks_.antibotBan && server->hooks_.antibotBan(address, true));
        return 1;
    });
    lua.setField("ban");
    wire::lua::pushHostFunction(lua, "antibot.unban", [server](LuaArgs& args) {
        const std::string address = args.checkString(1);
        args.pushBoolean(server->hooks_.antibotBan && server->hooks_.antibotBan(address, false));
        return 1;
    });
    lua.setField("unban");
    wire::lua::pushHostFunction(lua, "antibot.under_attack", [server](LuaArgs& args) {
        args.pushBoolean(server->hooks_.antibotAttack && server->hooks_.antibotAttack());
        return 1;
    });
    lua.setField("under_attack");
    lua.setGlobal("antibot");

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
    wire::lua::pushHostFunction(lua, "servers.address", [server](LuaArgs& args) {
        const auto found =
            server->hooks_.serverAddress ? server->hooks_.serverAddress(args.checkString(1)) : std::nullopt;
        if (!found)
            return 0;
        args.pushString(found->first);
        args.pushInteger(found->second);
        return 2;
    });
    lua.setField("address");
    wire::lua::pushHostFunction(lua, "servers.players", [server, pushOne](LuaArgs& args) {
        args.pushTable();
        std::int64_t position = 1;
        for (const std::string& name : server->playersOn(args.checkString(1))) {
            pushOne(args, server->findPlayer(name));
            args.setIndex(position++);
        }
        return 1;
    });
    lua.setField("players");
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

    wire::lua::pushHostFunction(lua, "has_permission", [server](LuaArgs& args) {
        std::string who;
        if (args.isTable(1)) {
            args.pushField(1, "name");
            who = args.isString(-1) ? args.toText(-1) : std::string{};
            args.pop(1);
        } else {
            who = args.checkString(1);
        }
        const std::string node = args.checkString(2);
        args.pushBoolean(server->hooks_.hasPermission && server->hooks_.hasPermission(who, node));
        return 1;
    });
    lua.setGlobal("has_permission");

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
        {
            std::lock_guard lock{server->mutex_};
            server->timers_.push_back(std::make_unique<PluginHost::Timer>(std::move(timer)));
        }
        server->wake_.notify_one();
    };
    const auto timerHandle = [schedule](PluginHost::Timer timer) {
        timer.cancelled = std::make_shared<std::atomic<bool>>(false);
        const auto cancelled = timer.cancelled;
        schedule(std::move(timer));
        return wire::plugins::PluginHandle{[cancelled] { cancelled->store(true); }};
    };
    wire::plugins::SchedulingHost scheduling;
    scheduling.wait = [schedule](std::int32_t ticks, wire::lua::LuaCall::Resume resume) {
        PluginHost::Timer timer;
        timer.due = std::chrono::steady_clock::now() + kTick * ticks;
        timer.resume = std::move(resume);
        schedule(std::move(timer));
    };
    scheduling.later = [timerHandle](std::int32_t ticks, std::shared_ptr<LuaRef> function) {
        PluginHost::Timer timer;
        timer.due = std::chrono::steady_clock::now() + kTick * ticks;
        timer.function = std::move(function);
        return timerHandle(std::move(timer));
    };
    scheduling.every = [timerHandle](std::int32_t delay, std::int32_t period, std::shared_ptr<LuaRef> function) {
        PluginHost::Timer timer;
        timer.due = std::chrono::steady_clock::now() + kTick * delay;
        timer.period = kTick * period;
        timer.function = std::move(function);
        return timerHandle(std::move(timer));
    };
    wire::plugins::installScheduling(*plugin.sandbox, lua, scheduling);

    wire::plugins::ListenerHost listeners;
    listeners.filterFields = [](std::string_view name) -> std::expected<std::vector<std::string>, std::string> {
        if (name != kJoinEvent && name != kQuitEvent && name != kServerChangeEvent && name != kChatEvent &&
            name != kKickEvent && name != kPingEvent && name != kLoginEvent && name != kMessageEvent &&
            name != kPreConnectEvent && name != kCommandEvent && name != kAntibotEvent)
            return std::unexpected(
                "there is no event called '" + std::string{name} + "'; this proxy has plugin.enable, plugin.disable, " +
                std::string{kJoinEvent} + ", " + std::string{kQuitEvent} + ", " + std::string{kServerChangeEvent} +
                ", " + std::string{kChatEvent} + ", " + std::string{kKickEvent} + ", " + std::string{kPingEvent} +
                ", " + std::string{kLoginEvent} + ", " + std::string{kMessageEvent} + ", " +
                std::string{kPreConnectEvent} + ", " + std::string{kCommandEvent} + ", " + std::string{kAntibotEvent});
        return std::vector<std::string>{"player_name", "player_uuid", "server", "from_server", "channel"};
    };
    listeners.lifecycle = [self](bool enable, LuaRef function) {
        (enable ? self->onEnable : self->onDisable).push_back(std::move(function));
    };
    listeners.add = [self](wire::plugins::ListenerRequest request) -> std::optional<std::string> {
        PluginHost::Plugin::Handler handler;
        handler.observer = request.observer;
        handler.function = std::move(request.function);
        handler.priority = request.options.priority;
        for (const auto& [field, value] : request.options.filters)
            if (const auto* text = std::get_if<std::string>(&value))
                handler.filters.emplace(field, *text);
            else if (const auto* number = std::get_if<std::int64_t>(&value))
                handler.filters.emplace(field, std::to_string(*number));
        if (!request.observer)
            self->host->deciders_.fetch_or(decisionBit(request.event));
        auto& list = self->handlers[request.event];
        const auto position =
            std::ranges::upper_bound(list, handler.priority, {}, &PluginHost::Plugin::Handler::priority);
        list.insert(position, std::move(handler));
        return std::nullopt;
    };
    wire::plugins::installListeners(lua, listeners);

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
        if (!server->hooks_.commands && std::ranges::find(kBuiltinCommands, root) != kBuiltinCommands.end())
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
        if (server->hooks_.commands) {
            runtime::CommandRegistration registration;
            registration.root = nodeOf(entry->spec, [server](const runtime::CommandContext& context) {
                if (context.source().console)
                    static_cast<void>(server->command(context.line()));
                else
                    static_cast<void>(server->playerCommand(context.source().name, context.line()));
            });
            registration.permission = entry->spec.permission.value_or(std::string{});
            registration.description = "Command of the plugin " + self->name + ".";
            registration.owner = "plugin:" + self->id + "/";
            if (auto added = server->hooks_.commands->add(std::move(registration)); !added)
                return args.fail(added.error());
        }
        entries.push_back(std::move(entry));
        return 0;
    });
    lua.setGlobal("cmd");

    if (manifest.capabilities.packets && server->settings_.packets) {
        lua.pushTable();
        for (const bool observer : {false, true}) {
            wire::lua::pushHostFunction(
                lua, observer ? "packet.watch" : "packet.on", [server, self, observer](LuaArgs& args) {
                    const std::string name = args.checkString(1);
                    const bool hasOptions = args.count() >= 3;
                    const std::int32_t at = hasOptions ? 3 : 2;
                    if (!args.isFunction(at))
                        return args.fail("packet.on(name, fn) needs a function");
                    const auto scope = wire::plugins::packetNaming::scopeOf(name);
                    if (!scope)
                        return args.fail("this proxy has no packet called '" + name +
                                         "'. Names look like clientbound.set_title_text, serverbound.play.* or "
                                         "clientbound.configuration.0x07");
                    wire::plugins::EventPriority priority = wire::plugins::EventPriority::Normal;
                    bool failClosed = false;
                    if (hasOptions && args.isTable(2)) {
                        args.pushField(2, "fail");
                        if (args.isString(-1)) {
                            const std::string written = args.toText(-1);
                            args.pop(1);
                            if (written != "open" && written != "closed")
                                return args.fail("fail is \"open\" or \"closed\", not '" + written + "'");
                            failClosed = written == "closed";
                        } else {
                            args.pop(1);
                        }
                        args.pushField(2, "priority");
                        if (args.isString(-1)) {
                            const std::string written = args.toText(-1);
                            args.pop(1);
                            const auto spelled = wire::plugins::priorityOf(written);
                            if (!spelled)
                                return args.fail("'" + written + "' is not a priority");
                            priority = *spelled;
                        } else {
                            args.pop(1);
                        }
                    }
                    self->packetHandles.push_back(
                        server->settings_.packets->add(self, *scope, LuaRef::of(args.state(), at),
                                                       static_cast<std::int32_t>(priority), observer, failClosed));
                    return 0;
                });
            lua.setField(observer ? "watch" : "on");
        }
        for (const bool inbound : {false, true}) {
            wire::lua::pushHostFunction(
                lua, inbound ? "packet.receive" : "packet.send", [server, inbound](LuaArgs& args) {
                    const std::string verb = inbound ? "packet.receive" : "packet.send";
                    std::optional<std::string> player =
                        args.isTable(1) ? args.fieldAsText(1, "name") : args.stringOrNull(1);
                    if (!player)
                        return args.fail(verb + "(player, name, bytes) needs a player");
                    const std::string name = args.checkString(2);
                    const auto address = wire::plugins::packetNaming::resolve(name);
                    if (!address || address->serverbound != inbound)
                        return args.fail("'" + name + "' is not a " + (inbound ? "serverbound" : "clientbound") +
                                         " packet");
                    if (!args.isTable(3))
                        return args.fail(verb + " needs the body as a list of bytes");
                    const std::int64_t length = args.length(3);
                    if (length < 0 || static_cast<std::size_t>(length) > server->settings_.packetMaxBytes)
                        return args.fail("a packet body is limited to " +
                                         std::to_string(server->settings_.packetMaxBytes) + " bytes here");
                    std::vector<std::uint8_t> bytes;
                    bytes.reserve(static_cast<std::size_t>(length));
                    for (std::int64_t position = 1; position <= length; position++) {
                        args.pushIndex(3, position);
                        bytes.push_back(static_cast<std::uint8_t>(args.toInteger(-1) & 0xFF));
                        args.pop(1);
                    }
                    args.pushBoolean(server->hooks_.packet &&
                                     server->hooks_.packet(*player, inbound, static_cast<std::int32_t>(address->phase),
                                                           address->id, bytes));
                    return 1;
                });
            lua.setField(inbound ? "receive" : "send");
        }
        lua.setGlobal("packet");
    }

    wire::plugins::pushDataScope(
        lua, dataScope(server->database_ ? &*server->database_ : nullptr, plugin.id, std::string{kServerScope},
                       manifest.capabilities.storage, "store needs storage = true under [capabilities] in plugin.toml",
                       "a plugin's store is limited to 1000 keys", true));
    lua.setGlobal("store");
}

} // namespace basalt::proxy::plugins
