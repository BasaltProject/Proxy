#include "WasmImports.hpp"

#include <basalt/wire/text/MiniMessage.hpp>

#include <algorithm>
#include <charconv>

namespace basalt::proxy::plugins::wasmimport {
namespace {

using namespace wire::wasm;

constexpr std::array<std::string_view, 3> kChatModes{"full", "system", "hidden"};
constexpr std::array<std::string_view, 3> kParticles{"all", "decreased", "minimal"};

[[nodiscard]] std::string_view pick(const auto& names, std::int32_t value) {
    return names[static_cast<std::size_t>(std::clamp<std::int32_t>(value, 0, 2))];
}

[[nodiscard]] bool serverExists(WasmHost& host, std::string_view name) {
    return host.hooks().servers && std::ranges::contains(host.hooks().servers(), name);
}

// ---- events

void eventOn(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto name = text(call, 0);
    const auto event = name ? wasmEventOf(*name) : std::nullopt;
    if (!event || call.i32(2) < 0 || call.i32(2) > 4)
        return call.ret(kErrInvalid);
    auto& list = plugin.handlers[static_cast<std::size_t>(*event)];
    const WasmHandler handler{call.i32(2), call.i32(3)};
    list.insert(std::ranges::upper_bound(list, handler.priority, {}, &WasmHandler::priority), handler);
    if (const unsigned bit = decisionBit(*name); bit != 0)
        plugin.host->addDecider(bit);
    call.ret(kOk);
}

void eventPlayer(void* user, Call& call) {
    WasmHost& host = *self(user).host;
    const WasmCall* live = host.live(call.i32(0));
    if (live == nullptr)
        return call.ret(kErrStale);
    call.ret(live->console || live->name.empty() ? 0 : host.handleOf(live->name));
}

[[nodiscard]] std::optional<std::string_view> fieldText(const WasmCall& live, std::string_view field) {
    const WasmDecision* decision = live.decision;
    if (field == "player_name")
        return live.name.empty() ? std::nullopt : std::optional{live.name};
    if (field == "player_uuid")
        return live.uuid.empty() ? std::nullopt : std::optional{live.uuid};
    if (field == "address")
        return live.address.empty() ? std::nullopt : std::optional{live.address};
    if (field == "from_server")
        return live.from.empty() && decision == nullptr ? std::nullopt : std::optional{live.from};
    if (field == "server") {
        if (decision != nullptr && decision->kind == runtime::DecisionKind::kPreConnect)
            return decision->server;
        return live.server.empty() ? std::nullopt : std::optional{live.server};
    }
    if (decision == nullptr)
        return std::nullopt;
    using Kind = runtime::DecisionKind;
    if (field == "host")
        return decision->host;
    if (field == "reason" && decision->kind != Kind::kPing)
        return std::string_view{decision->reason};
    if (decision->kind != Kind::kPing)
        return std::nullopt;
    if (field == "protocol")
        return decision->protocol;
    if (field == "motd")
        return std::string_view{decision->motd};
    if (field == "version")
        return std::string_view{decision->version};
    if (field == "sample")
        return std::string_view{decision->sample};
    if (field == "favicon")
        return std::string_view{decision->favicon};
    return std::nullopt;
}

void eventFieldStr(void* user, Call& call) {
    const WasmCall* live = self(user).host->live(call.i32(0));
    const auto field = text(call, 1);
    if (live == nullptr)
        return call.ret(kErrStale);
    const auto value = field ? fieldText(*live, *field) : std::nullopt;
    call.ret(value ? call.memory().put(call.i32(3), call.i32(4), *value) : kErrAbsent);
}

void eventFieldI64(void* user, Call& call) {
    const WasmCall* live = self(user).host->live(call.i32(0));
    const auto field = text(call, 1);
    if (live == nullptr)
        return call.ret(kErrStale);
    std::optional<std::int64_t> value;
    const WasmDecision* decision = live->decision;
    if (field && *field == "port" && !live->address.empty())
        value = live->port;
    else if (field && decision != nullptr) {
        if (*field == "cancelled")
            value = decision->cancelled ? 1 : 0;
        else if (*field == "protocol" && decision->kind == runtime::DecisionKind::kPing) {
            std::int64_t number = 0;
            if (std::from_chars(decision->protocol.data(), decision->protocol.data() + decision->protocol.size(),
                                number)
                    .ec == std::errc{})
                value = number;
        } else if (decision->kind == runtime::DecisionKind::kPing) {
            if (*field == "max_players")
                value = decision->maxPlayers;
            else if (*field == "online")
                value = decision->online;
            else if (*field == "version_protocol")
                value = decision->versionProtocol;
        }
    }
    if (!value)
        return call.ret(kErrAbsent);
    call.ret(call.memory().write(call.i32(3), *value) ? kOk : kErrInvalid);
}

void eventCancel(void* user, Call& call) {
    WasmCall* live = self(user).host->live(call.i32(0));
    if (live == nullptr)
        return call.ret(kErrStale);
    if (live->decision == nullptr || live->decision->kind == runtime::DecisionKind::kPing ||
        live->decision->kind == runtime::DecisionKind::kKick)
        return call.ret(kErrInvalid);
    live->decision->cancelled = call.i32(1) != 0;
    call.ret(kOk);
}

void eventSetStr(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    WasmCall* live = plugin.host->live(call.i32(0));
    const auto field = text(call, 1);
    const auto value = text(call, 3);
    if (live == nullptr)
        return call.ret(kErrStale);
    if (live->decision == nullptr || !field || !value)
        return call.ret(kErrInvalid);
    WasmDecision& decision = *live->decision;
    using Kind = runtime::DecisionKind;
    if (*field == "reason" && decision.kind != Kind::kPing) {
        const auto line = value->starts_with('@')
                              ? plugin.text->translation(value->substr(1)).value_or(std::string{value->substr(1)})
                              : std::string{*value};
        decision.reason = line.substr(0, kWasmMaxText);
        decision.touched |= WasmDecision::kReason;
    } else if (*field == "server" && (decision.kind == Kind::kPreConnect || decision.kind == Kind::kKick)) {
        if (!serverExists(*plugin.host, *value))
            return call.ret(kErrInvalid);
        decision.server = std::string{*value};
        decision.touched |= WasmDecision::kServer;
    } else if (decision.kind == Kind::kPing && *field == "motd") {
        decision.motd = std::string{value->substr(0, kWasmMaxText)};
        decision.touched |= WasmDecision::kMotd;
    } else if (decision.kind == Kind::kPing && *field == "version") {
        decision.version = std::string{value->substr(0, kMaxSampleLine)};
        decision.touched |= WasmDecision::kVersion;
    } else if (decision.kind == Kind::kPing && *field == "sample") {
        decision.sample = std::string{value->substr(0, kMaxSampleLines * (kMaxSampleLine + 1))};
        decision.touched |= WasmDecision::kSample;
    } else if (decision.kind == Kind::kPing && *field == "favicon") {
        decision.favicon = std::string{*value};
        decision.touched |= WasmDecision::kFavicon;
    } else {
        return call.ret(kErrInvalid);
    }
    call.ret(kOk);
}

void eventSetI64(void* user, Call& call) {
    WasmCall* live = self(user).host->live(call.i32(0));
    const auto field = text(call, 1);
    if (live == nullptr)
        return call.ret(kErrStale);
    if (live->decision == nullptr || live->decision->kind != runtime::DecisionKind::kPing || !field)
        return call.ret(kErrInvalid);
    WasmDecision& decision = *live->decision;
    const std::int64_t value = call.i64(3);
    if (*field == "max_players" && value >= 0) {
        decision.maxPlayers = value;
        decision.touched |= WasmDecision::kMaxPlayers;
    } else if (*field == "online" && value >= 0) {
        decision.online = value;
        decision.touched |= WasmDecision::kOnline;
    } else if (*field == "version_protocol" && value >= 0) {
        decision.versionProtocol = value;
        decision.touched |= WasmDecision::kVersionProtocol;
    } else {
        return call.ret(kErrInvalid);
    }
    call.ret(kOk);
}

// ---- commands

void commandRegister(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    WasmHost& wasm = *plugin.host;
    PluginHost& host = wasm.host();
    const auto spec = text(call, 0);
    const auto permission = text(call, 2);
    if (!spec || !permission)
        return call.ret(kErrInvalid);
    auto parsed = wire::plugins::CommandSpec::parse(
        *spec, permission->empty() ? std::nullopt : std::optional<std::string>{std::string{*permission}});
    if (!parsed)
        return call.ret(kErrInvalid);
    const std::string root = parsed->name();
    const bool valid = root.size() <= 32 && std::ranges::all_of(root, [](char c) {
                           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
                       });
    if (!valid)
        return call.ret(kErrInvalid);
    std::unique_lock lock{wasm.commandsMutex()};
    auto& entries = wasm.commands_[root];
    if (std::ranges::any_of(entries, [&](const auto& other) { return other.spec.literals == parsed->literals; }))
        return call.ret(kErrInvalid);
    if (wasm.hooks().commands) {
        runtime::CommandRegistration registration;
        registration.root = commandNode(*parsed, [&host](const runtime::CommandContext& context) {
            if (context.source().console)
                static_cast<void>(host.command(context.line()));
            else
                static_cast<void>(host.playerCommand(context.source().name, context.line()));
        });
        registration.permission = parsed->permission.value_or(std::string{});
        registration.description = "Command of the plugin " + plugin.name + ".";
        registration.owner = "plugin:" + plugin.id + "/";
        if (auto added = wasm.hooks().commands->add(std::move(registration)); !added) {
            if (entries.empty())
                wasm.commands_.erase(root);
            return call.ret(kErrInvalid);
        }
    }
    entries.push_back(WasmCommand{&plugin, std::move(*parsed), call.i32(4)});
    call.ret(kOk);
}

[[nodiscard]] WasmCall* commandCall(void* user, Call& call) {
    WasmCall* live = self(user).host->live(call.i32(0));
    return live != nullptr && live->command ? live : nullptr;
}

void commandSourceName(void* user, Call& call) {
    const WasmCall* live = commandCall(user, call);
    call.ret(live != nullptr ? call.memory().put(call.i32(1), call.i32(2), live->name) : kErrStale);
}

void commandIsConsole(void* user, Call& call) {
    const WasmCall* live = commandCall(user, call);
    call.ret(live == nullptr ? kErrStale : static_cast<std::int32_t>(live->console));
}

void commandReply(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const WasmCall* live = commandCall(user, call);
    if (live == nullptr)
        return call.ret(kErrStale);
    const auto line = resolve(plugin, call, 1, 3);
    if (!line)
        return call.ret(kErrInvalid);
    if (!live->console && plugin.host->hooks().tell)
        plugin.host->hooks().tell(live->name, *line);
    else if (live->console)
        plugin.host->logLine(plugin, "", *line);
    call.ret(kOk);
}

[[nodiscard]] const wire::plugins::CommandValue* commandValue(const WasmCall& live, std::string_view name) {
    for (std::size_t index = 0; index < live.spec->arguments.size() && index < live.values->size(); ++index)
        if (live.spec->arguments[index].name == name)
            return &(*live.values)[index];
    return nullptr;
}

void commandArgStr(void* user, Call& call) {
    const WasmCall* live = commandCall(user, call);
    const auto name = text(call, 1);
    if (live == nullptr)
        return call.ret(kErrStale);
    const auto* value = name ? commandValue(*live, *name) : nullptr;
    if (value == nullptr || std::holds_alternative<std::monostate>(*value))
        return call.ret(kErrAbsent);
    std::string rendered;
    if (const auto* line = std::get_if<std::string>(value))
        rendered = *line;
    else if (const auto* whole = std::get_if<std::int64_t>(value))
        rendered = std::to_string(*whole);
    else if (const auto* real = std::get_if<double>(value))
        rendered = std::to_string(*real);
    else
        rendered = std::get<bool>(*value) ? "true" : "false";
    call.ret(call.memory().put(call.i32(3), call.i32(4), rendered));
}

void commandArgI64(void* user, Call& call) {
    const WasmCall* live = commandCall(user, call);
    const auto name = text(call, 1);
    if (live == nullptr)
        return call.ret(kErrStale);
    const auto* value = name ? commandValue(*live, *name) : nullptr;
    if (value == nullptr || std::holds_alternative<std::monostate>(*value))
        return call.ret(kErrAbsent);
    const auto* whole = std::get_if<std::int64_t>(value);
    if (whole == nullptr)
        return call.ret(kErrInvalid);
    call.ret(call.memory().write(call.i32(3), *whole) ? kOk : kErrInvalid);
}

// ---- tasks

void taskAfter(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    WasmHost& wasm = *plugin.host;
    if (call.i32(0) < 0)
        return call.ret(kErrInvalid);
    wasm.timers_.push_back(
        WasmTimer{&plugin, wasm.nextTimer_, call.i32(1), std::chrono::steady_clock::now() + kTick * call.i32(0), {}});
    call.ret(wasm.nextTimer_++);
}

void taskEvery(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    WasmHost& wasm = *plugin.host;
    if (call.i32(0) < 0 || call.i32(1) < 1)
        return call.ret(kErrInvalid);
    wasm.timers_.push_back(WasmTimer{&plugin, wasm.nextTimer_, call.i32(2),
                                     std::chrono::steady_clock::now() + kTick * call.i32(0), kTick * call.i32(1)});
    call.ret(wasm.nextTimer_++);
}

void taskCancel(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto removed = std::erase_if(plugin.host->timers_, [&](const WasmTimer& timer) {
        return timer.id == call.i32(0) && timer.plugin == &plugin;
    });
    call.ret(removed > 0 ? kOk : kErrAbsent);
}

// ---- proxy players

void playerUuid(void* user, Call& call) {
    const WasmHost::Player* player = self(user).host->playerOf(call.i32(0));
    call.ret(player != nullptr ? call.memory().put(call.i32(1), call.i32(2), player->uuid) : kErrStale);
}

void playerServer(void* user, Call& call) {
    const WasmHost::Player* player = self(user).host->playerOf(call.i32(0));
    call.ret(player != nullptr ? call.memory().put(call.i32(1), call.i32(2), player->server) : kErrStale);
}

void playerPing(void* user, Call& call) {
    const WasmHost::Player* player = self(user).host->playerOf(call.i32(0));
    if (player == nullptr)
        return call.ret(kErrStale);
    call.ret(player->ping < 0 ? kErrAbsent
                              : static_cast<std::int32_t>(std::min<std::int64_t>(player->ping, INT32_MAX)));
}

void playerSettingStr(void* user, Call& call) {
    const WasmHost::Player* player = self(user).host->playerOf(call.i32(0));
    const auto field = text(call, 1);
    if (player == nullptr)
        return call.ret(kErrStale);
    if (!player->settings || !field)
        return call.ret(kErrAbsent);
    const auto& settings = *player->settings;
    std::string_view value;
    if (*field == "locale")
        value = settings.locale;
    else if (*field == "chat_mode")
        value = pick(kChatModes, settings.chatMode);
    else if (*field == "main_hand")
        value = settings.mainHand == 0 ? "left" : "right";
    else if (*field == "particles")
        value = pick(kParticles, settings.particles);
    else
        return call.ret(kErrInvalid);
    call.ret(call.memory().put(call.i32(3), call.i32(4), value));
}

void playerSettingI64(void* user, Call& call) {
    const WasmHost::Player* player = self(user).host->playerOf(call.i32(0));
    const auto field = text(call, 1);
    if (player == nullptr)
        return call.ret(kErrStale);
    if (!player->settings || !field)
        return call.ret(kErrAbsent);
    const auto& settings = *player->settings;
    std::int64_t value = 0;
    if (*field == "view_distance")
        value = settings.viewDistance;
    else if (*field == "chat_colors")
        value = settings.chatColors;
    else if (*field == "skin_parts")
        value = settings.skinParts;
    else if (*field == "text_filtering")
        value = settings.textFiltering;
    else if (*field == "server_listing")
        value = settings.serverListing;
    else
        return call.ret(kErrInvalid);
    call.ret(call.memory().write(call.i32(3), value) ? kOk : kErrInvalid);
}

void playerHasPermission(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const WasmHost::Player* player = wasm.playerOf(call.i32(0));
    const auto node = text(call, 1);
    if (player == nullptr)
        return call.ret(kErrStale);
    call.ret(node && wasm.hooks().hasPermission && wasm.hooks().hasPermission(player->name, *node) ? 1 : 0);
}

[[nodiscard]] std::optional<wire::text::Component> partOf(const WasmPlugin& plugin, std::string_view part) {
    if (part.empty())
        return std::nullopt;
    const std::string line = part.starts_with('@')
                                 ? plugin.text->translation(part.substr(1)).value_or(std::string{part})
                                 : std::string{part};
    return wire::text::miniMessage::parse(line.substr(0, kWasmMaxText));
}

void playerTitle(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const WasmHost::Player* player = plugin.host->playerOf(call.i32(0));
    const auto title = text(call, 1);
    const auto subtitle = text(call, 3);
    if (player == nullptr)
        return call.ret(kErrStale);
    if (!title || !subtitle)
        return call.ret(kErrInvalid);
    runtime::HudTitle hud;
    hud.title = partOf(plugin, *title);
    hud.subtitle = partOf(plugin, *subtitle);
    if (call.i32(5) >= 0 || call.i32(6) >= 0 || call.i32(7) >= 0)
        hud.times = {call.i32(5) < 0 ? 10 : std::min(call.i32(5), 72000),
                     call.i32(6) < 0 ? 70 : std::min(call.i32(6), 72000),
                     call.i32(7) < 0 ? 20 : std::min(call.i32(7), 72000)};
    call.ret(plugin.host->hooks().hud && plugin.host->hooks().hud(player->name, std::move(hud)) ? kOk : kErrInvalid);
}

void playerConnect(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const WasmHost::Player* player = wasm.playerOf(call.i32(0));
    const auto server = text(call, 1);
    if (player == nullptr)
        return call.ret(kErrStale);
    call.ret(server && wasm.hooks().send && wasm.hooks().send(player->name, *server) ? kOk : kErrInvalid);
}

void playerKick(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const WasmHost::Player* player = plugin.host->playerOf(call.i32(0));
    if (player == nullptr)
        return call.ret(kErrStale);
    const auto line = resolve(plugin, call, 1, 3);
    if (!line)
        return call.ret(kErrInvalid);
    if (plugin.host->hooks().kick)
        plugin.host->hooks().kick(player->name, *line);
    call.ret(kOk);
}

// ---- backends

void serversList(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    std::string joined;
    if (wasm.hooks().servers)
        for (const std::string& name : wasm.hooks().servers())
            joined += (joined.empty() ? "" : "\n") + name;
    call.ret(call.memory().put(call.i32(0), call.i32(1), joined));
}

void serverAddress(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const auto name = text(call, 0);
    const auto found = name && wasm.hooks().serverAddress ? wasm.hooks().serverAddress(*name) : std::nullopt;
    if (!found)
        return call.ret(kErrAbsent);
    call.ret(call.memory().put(call.i32(2), call.i32(3), found->first + ":" + std::to_string(found->second)));
}

void playersOn(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const auto server = text(call, 0);
    if (!server)
        return call.ret(kErrInvalid);
    std::vector<std::int32_t> handles;
    for (const auto& [name, player] : wasm.roster())
        if (player.server == *server)
            handles.push_back(wasm.handleOf(name));
    const auto count = static_cast<std::int32_t>(handles.size());
    if (call.i32(3) < count || count == 0)
        return call.ret(count);
    const auto view =
        call.memory().range(static_cast<std::uint32_t>(call.i32(2)), static_cast<std::uint32_t>(count) * 4);
    if (!view)
        return call.ret(kErrInvalid);
    std::memcpy(view->data(), handles.data(), handles.size() * 4);
    call.ret(count);
}

constexpr std::array kProxy = std::to_array<Entry>({
    {kBasaltModule, "event_on", eventOn},
    {kBasaltModule, "event_player", eventPlayer},
    {kBasaltModule, "event_field_str", eventFieldStr},
    {kBasaltModule, "event_field_i64", eventFieldI64},
    {kBasaltModule, "event_cancel", eventCancel},
    {kBasaltModule, "command_register", commandRegister},
    {kBasaltModule, "command_source_name", commandSourceName},
    {kBasaltModule, "command_is_console", commandIsConsole},
    {kBasaltModule, "command_reply", commandReply},
    {kBasaltModule, "command_arg_str", commandArgStr},
    {kBasaltModule, "command_arg_i64", commandArgI64},
    {kBasaltModule, "task_after", taskAfter},
    {kBasaltModule, "task_every", taskEvery},
    {kBasaltModule, "task_cancel", taskCancel},
    {kProxyModule, "event_set_str", eventSetStr},
    {kProxyModule, "event_set_i64", eventSetI64},
    {kProxyModule, "player_uuid", playerUuid},
    {kProxyModule, "player_server", playerServer},
    {kProxyModule, "player_ping", playerPing},
    {kProxyModule, "player_setting_str", playerSettingStr},
    {kProxyModule, "player_setting_i64", playerSettingI64},
    {kProxyModule, "player_has_permission", playerHasPermission},
    {kProxyModule, "player_title", playerTitle},
    {kProxyModule, "player_connect", playerConnect},
    {kProxyModule, "player_kick", playerKick},
    {kProxyModule, "servers_list", serversList},
    {kProxyModule, "server_address", serverAddress},
    {kProxyModule, "players_on", playersOn},
});

} // namespace

void addProxyImports(std::vector<Entry>& out) { out.insert(out.end(), kProxy.begin(), kProxy.end()); }

} // namespace basalt::proxy::plugins::wasmimport
