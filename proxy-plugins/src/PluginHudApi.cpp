#include "PluginInternal.hpp"

#include <basalt/wire/text/MiniMessage.hpp>

#include <algorithm>
#include <array>

namespace basalt::proxy::plugins {
namespace {

using wire::lua::LuaArgs;

constexpr std::size_t kMaxText = 4096;
constexpr std::size_t kMaxBarId = 64;
constexpr std::array<std::string_view, 7> kColors{"pink", "blue", "red", "green", "yellow", "purple", "white"};
constexpr std::array<std::string_view, 5> kOverlays{"progress", "notched_6", "notched_10", "notched_12", "notched_20"};

std::int32_t indexOf(const auto& names, std::string_view name, std::int32_t fallback) {
    const auto found = std::ranges::find(names, name);
    return found == names.end() ? fallback : static_cast<std::int32_t>(found - names.begin());
}

} // namespace

void PluginApi::installHud(PluginHost& host, PluginHost::Plugin& plugin) {
    LuaArgs lua{plugin.sandbox->state()};
    PluginHost* server = &host;
    PluginHost::Plugin* self = &plugin;
    plugin.playerMeta.push(lua);
    lua.pushField(-1, "__index");

    const auto text = [self](LuaArgs& args, std::int32_t at) {
        return wire::text::miniMessage::parse(
            self->text->resolve(args.checkString(at), &args, at + 1).substr(0, kMaxText));
    };
    const auto optionalText = [](LuaArgs& args, std::int32_t at) -> std::optional<wire::text::Component> {
        if (args.isNil(at) || !args.isString(at))
            return std::nullopt;
        return wire::text::miniMessage::parse(args.checkString(at).substr(0, kMaxText));
    };
    const auto player = [](LuaArgs& args, std::string_view) { return args.fieldAsText(1, "name"); };

    wire::lua::pushHostFunction(lua, "title", [server, optionalText, player](LuaArgs& args) {
        const auto name = player(args, "title");
        if (!name)
            return args.fail("this is a player method: call it as player:method(...)");
        runtime::HudTitle title;
        title.title = optionalText(args, 2);
        title.subtitle = optionalText(args, 3);
        if (args.count() >= 4)
            title.times = {static_cast<std::int32_t>(std::clamp<std::int64_t>(args.integerOr(4, 10), 0, 72000)),
                           static_cast<std::int32_t>(std::clamp<std::int64_t>(args.integerOr(5, 70), 0, 72000)),
                           static_cast<std::int32_t>(std::clamp<std::int64_t>(args.integerOr(6, 20), 0, 72000))};
        args.pushBoolean(server->hooks_.hud && server->hooks_.hud(*name, std::move(title)));
        return 1;
    });
    lua.setField("title");
    wire::lua::pushHostFunction(lua, "clear_title", [server, player](LuaArgs& args) {
        const auto name = player(args, "clear_title");
        if (!name)
            return args.fail("this is a player method: call it as player:method(...)");
        runtime::HudTitle title;
        title.clear = true;
        title.reset = !args.isNil(2) && args.toBoolean(2);
        args.pushBoolean(server->hooks_.hud && server->hooks_.hud(*name, std::move(title)));
        return 1;
    });
    lua.setField("clear_title");
    wire::lua::pushHostFunction(lua, "action_bar", [server, text, player](LuaArgs& args) {
        const auto name = player(args, "action_bar");
        if (!name)
            return args.fail("this is a player method: call it as player:method(...)");
        runtime::HudTitle title;
        title.actionBar = text(args, 2);
        args.pushBoolean(server->hooks_.hud && server->hooks_.hud(*name, std::move(title)));
        return 1;
    });
    lua.setField("action_bar");
    wire::lua::pushHostFunction(lua, "tab_list", [server, optionalText, player](LuaArgs& args) {
        const auto name = player(args, "tab_list");
        if (!name)
            return args.fail("this is a player method: call it as player:method(...)");
        runtime::HudTabList tab;
        tab.header = optionalText(args, 2);
        tab.footer = optionalText(args, 3);
        args.pushBoolean(server->hooks_.hud && server->hooks_.hud(*name, std::move(tab)));
        return 1;
    });
    lua.setField("tab_list");

    const auto bar = [server, player](LuaArgs& args, std::string_view method, runtime::HudBossBar::Op op,
                                      const std::function<void(runtime::HudBossBar&, LuaArgs&)>& fill) {
        const auto name = player(args, method);
        if (!name)
            return args.fail("this is a player method: call it as player:method(...)");
        runtime::HudBossBar change;
        change.op = op;
        change.id = args.checkString(2);
        if (change.id.empty() || change.id.size() > kMaxBarId)
            return args.fail("a boss bar id is 1 to 64 characters");
        if (fill)
            fill(change, args);
        args.pushBoolean(server->hooks_.hud && server->hooks_.hud(*name, std::move(change)));
        return 1;
    };
    wire::lua::pushHostFunction(lua, "show_bossbar", [bar, text](LuaArgs& args) {
        return bar(args, "show_bossbar", runtime::HudBossBar::Op::kAdd, [&](runtime::HudBossBar& change, LuaArgs& in) {
            change.name = text(in, 3);
            change.progress = static_cast<float>(std::clamp(in.numberOr(4, 1.0), 0.0, 1.0));
            change.color = indexOf(kColors, in.stringOrNull(5).value_or("purple"), 5);
            change.overlay = indexOf(kOverlays, in.stringOrNull(6).value_or("progress"), 0);
        });
    });
    lua.setField("show_bossbar");
    wire::lua::pushHostFunction(lua, "hide_bossbar", [bar](LuaArgs& args) {
        return bar(args, "hide_bossbar", runtime::HudBossBar::Op::kRemove, nullptr);
    });
    lua.setField("hide_bossbar");
    wire::lua::pushHostFunction(lua, "bossbar_progress", [bar](LuaArgs& args) {
        return bar(args, "bossbar_progress", runtime::HudBossBar::Op::kProgress,
                   [](runtime::HudBossBar& change, LuaArgs& in) {
                       change.progress = static_cast<float>(std::clamp(in.numberOr(3, 1.0), 0.0, 1.0));
                   });
    });
    lua.setField("bossbar_progress");
    wire::lua::pushHostFunction(lua, "bossbar_title", [bar, text](LuaArgs& args) {
        return bar(args, "bossbar_title", runtime::HudBossBar::Op::kName,
                   [&](runtime::HudBossBar& change, LuaArgs& in) { change.name = text(in, 3); });
    });
    lua.setField("bossbar_title");
    wire::lua::pushHostFunction(lua, "bossbar_style", [bar](LuaArgs& args) {
        return bar(args, "bossbar_style", runtime::HudBossBar::Op::kStyle,
                   [](runtime::HudBossBar& change, LuaArgs& in) {
                       change.color = indexOf(kColors, in.stringOrNull(3).value_or("purple"), 5);
                       change.overlay = indexOf(kOverlays, in.stringOrNull(4).value_or("progress"), 0);
                   });
    });
    lua.setField("bossbar_style");

    wire::lua::pushHostFunction(lua, "transfer", [server, player](LuaArgs& args) {
        const auto name = player(args, "transfer");
        if (!name)
            return args.fail("this is a player method: call it as player:method(...)");
        runtime::HudTransfer transfer;
        transfer.host = args.checkString(2);
        transfer.port = static_cast<std::int32_t>(args.integerOr(3, 25565));
        args.pushBoolean(server->hooks_.hud && server->hooks_.hud(*name, std::move(transfer)));
        return 1;
    });
    lua.setField("transfer");
    wire::lua::pushHostFunction(lua, "sidebar", [server, text, player](LuaArgs& args) {
        const auto name = player(args, "sidebar");
        if (!name)
            return args.fail("this is a player method: call it as player:method(title, line, line, ...)");
        runtime::HudSidebar sidebar;
        sidebar.title = text(args, 2);
        for (std::int32_t at = 3; at <= args.count() && sidebar.lines.size() < 15; ++at)
            sidebar.lines.push_back(text(args, at));
        args.pushBoolean(server->hooks_.hud && server->hooks_.hud(*name, std::move(sidebar)));
        return 1;
    });
    lua.setField("sidebar");
    wire::lua::pushHostFunction(lua, "clear_sidebar", [server, player](LuaArgs& args) {
        const auto name = player(args, "clear_sidebar");
        if (!name)
            return args.fail("this is a player method: call it as player:method(...)");
        runtime::HudSidebar sidebar;
        sidebar.remove = true;
        args.pushBoolean(server->hooks_.hud && server->hooks_.hud(*name, std::move(sidebar)));
        return 1;
    });
    lua.setField("clear_sidebar");
    wire::lua::pushHostFunction(lua, "tab_entry", [server, text, player](LuaArgs& args) {
        const auto name = player(args, "tab_entry");
        if (!name)
            return args.fail("this is a player method: call it as player:method(id, name, text, latency)");
        runtime::HudTabEntry entry;
        entry.id = args.checkString(2).substr(0, kMaxBarId);
        entry.name = args.checkString(3);
        entry.text = text(args, 4);
        entry.latencyMillis = static_cast<std::int32_t>(std::clamp<std::int64_t>(args.integerOr(5, 0), 0, 10000));
        args.pushBoolean(server->hooks_.hud && server->hooks_.hud(*name, std::move(entry)));
        return 1;
    });
    lua.setField("tab_entry");
    wire::lua::pushHostFunction(lua, "remove_tab_entry", [server, player](LuaArgs& args) {
        const auto name = player(args, "remove_tab_entry");
        if (!name)
            return args.fail("this is a player method: call it as player:method(id)");
        runtime::HudTabEntry entry;
        entry.id = args.checkString(2).substr(0, kMaxBarId);
        entry.remove = true;
        args.pushBoolean(server->hooks_.hud && server->hooks_.hud(*name, std::move(entry)));
        return 1;
    });
    lua.setField("remove_tab_entry");
    wire::lua::pushHostFunction(lua, "set_permission", [server, player](LuaArgs& args) {
        const auto name = player(args, "set_permission");
        if (!name)
            return args.fail("this is a player method: call it as player:method(node, granted)");
        const std::string node = args.checkString(2);
        const int value = args.isNil(3) ? -1 : (args.toBoolean(3) ? 1 : 0);
        if (server->hooks_.setPermission)
            server->hooks_.setPermission(*name, node, value);
        return 0;
    });
    lua.setField("set_permission");
    wire::lua::pushHostFunction(lua, "group", [server, player](LuaArgs& args) {
        const auto name = player(args, "group");
        if (!name)
            return args.fail("this is a player method: call it as player:method()");
        args.pushString(server->hooks_.groupOf ? server->hooks_.groupOf(*name) : std::string{});
        return 1;
    });
    lua.setField("group");
    wire::lua::pushHostFunction(lua, "set_group", [server, player](LuaArgs& args) {
        const auto name = player(args, "set_group");
        if (!name)
            return args.fail("this is a player method: call it as player:method(group)");
        const std::string group = args.isNil(2) ? std::string{} : args.checkString(2);
        args.pushBoolean(server->hooks_.setGroup && server->hooks_.setGroup(*name, group));
        return 1;
    });
    lua.setField("set_group");
    wire::lua::pushHostFunction(lua, "permissions", [server, player](LuaArgs& args) {
        const auto name = player(args, "permissions");
        if (!name)
            return args.fail("this is a player method: call it as player:method()");
        const auto nodes =
            server->hooks_.permissionList ? server->hooks_.permissionList(*name) : std::vector<std::string>{};
        args.pushTable();
        for (std::size_t at = 0; at < nodes.size(); ++at) {
            args.pushString(nodes[at]);
            args.setIndex(static_cast<std::int64_t>(at + 1));
        }
        return 1;
    });
    lua.setField("permissions");
    wire::lua::pushHostFunction(lua, "chat", [server, player](LuaArgs& args) {
        const auto name = player(args, "chat");
        if (!name)
            return args.fail("this is a player method: call it as player:method(...)");
        args.pushBoolean(server->hooks_.spoofChat && server->hooks_.spoofChat(*name, args.checkString(2)));
        return 1;
    });
    lua.setField("chat");
    wire::lua::pushHostFunction(lua, "set_cookie", [server, player](LuaArgs& args) {
        const auto name = player(args, "set_cookie");
        if (!name)
            return args.fail("this is a player method: call it as player:method(...)");
        runtime::HudStoreCookie cookie;
        cookie.key = args.checkString(2);
        const std::string data = args.checkString(3);
        cookie.data.assign(data.begin(), data.end());
        args.pushBoolean(server->hooks_.hud && server->hooks_.hud(*name, std::move(cookie)));
        return 1;
    });
    lua.setField("set_cookie");
    wire::lua::pushHostFunction(lua, "cookie", [server, self, player](LuaArgs& args) {
        const auto name = player(args, "cookie");
        if (!name)
            return args.fail("this is a player method: call it as player:method(...)");
        const std::string key = args.checkString(2);
        if (!args.isFunction(3))
            return args.fail("cookie(key, fn) needs a function that receives the value, or nil");
        const auto function = std::make_shared<wire::lua::LuaRef>(wire::lua::LuaRef::of(args.state(), 3));
        const auto reply = [server, self, function](std::optional<std::vector<std::uint8_t>> data) {
            PluginHost::Timer timer;
            timer.due = std::chrono::steady_clock::now();
            timer.function = function;
            timer.plugin = self;
            timer.arguments = [data = std::move(data)](LuaArgs& in) {
                if (data)
                    in.pushString(std::string_view{reinterpret_cast<const char*>(data->data()), data->size()});
                else
                    in.pushNil();
                return 1;
            };
            {
                std::lock_guard lock{server->mutex_};
                server->timers_.push_back(std::make_unique<PluginHost::Timer>(std::move(timer)));
            }
            server->wake_.notify_one();
        };
        args.pushBoolean(server->hooks_.requestCookie && server->hooks_.requestCookie(*name, key, reply));
        return 1;
    });
    lua.setField("cookie");

    lua.pop(2);
}

} // namespace basalt::proxy::plugins
