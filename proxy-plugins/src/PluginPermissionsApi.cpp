#include "PluginInternal.hpp"

#include <basalt/wire/lua/LuaCall.hpp>
#include <basalt/wire/permissions/PermissionChain.hpp>

#include <mutex>

namespace basalt::proxy::plugins {
namespace {
using wire::lua::LuaArgs;
using wire::lua::LuaRef;
using wire::permissions::Context;
using wire::permissions::Subject;
using wire::permissions::Tristate;

void pushSubject(LuaArgs& lua, const Subject& subject) {
    lua.pushTable();
    lua.pushString(subject.name);
    lua.setField("name");
    lua.pushString(subject.uuid);
    lua.setField("uuid");
    lua.pushBoolean(subject.isConsole());
    lua.setField("console");
}

void pushContext(LuaArgs& lua, const Context& context) {
    lua.pushTable();
    lua.pushString(context.server);
    lua.setField("server");
    lua.pushString(context.world);
    lua.setField("world");
    for (const auto& entry : context.extra) {
        lua.pushString(entry.value);
        lua.setField(entry.key);
    }
}

struct Asked {
    std::string name;
    std::string uuid;
    std::string server;
    std::string world;
    bool console = false;
};

} // namespace

std::optional<std::string> PluginHost::callProvider(PermissionSlot& slot, bool meta,
                                                    const wire::permissions::Subject& subject, std::string_view text,
                                                    const wire::permissions::Context& context, Tristate& answer) {
    std::unique_lock<std::recursive_timed_mutex> turn{luaMutex_, std::defer_lock};
    if (!slot.alive.load(std::memory_order_acquire) || !turn.try_lock_for(settings_.packetTimeout))
        return std::nullopt;
    if (!slot.alive.load(std::memory_order_acquire) || slot.plugin->broken)
        return std::nullopt;
    Plugin& plugin = *slot.plugin;
    const LuaRef& function = meta ? slot.meta : slot.check;
    plugin.sandbox->refill();
    const auto answered = wire::lua::callSync(
        *plugin.sandbox, function,
        [&](LuaArgs& lua) {
            pushSubject(lua, subject);
            lua.pushString(text);
            pushContext(lua, context);
            return 3;
        },
        1);
    if (!answered) {
        report(plugin, answered.error(), meta ? "permission meta" : "permission provider");
        return std::nullopt;
    }
    plugin.strikes = 0;
    LuaArgs lua{plugin.sandbox->state()};
    std::optional<std::string> value;
    if (lua.isBoolean(-1))
        answer = wire::permissions::tristateOf(lua.toBoolean(-1));
    else if (lua.isString(-1))
        value = lua.toText(-1);
    lua.pop(*answered);
    return value;
}

void PluginHost::dropPermissionProviders() {
    for (auto& plugin : plugins_)
        plugin->permissionSlots.clear();
}

void PluginApi::installPermissions(PluginHost& host, PluginHost::Plugin& plugin) {
    PluginHost* server = &host;
    PluginHost::Plugin* self = &plugin;
    auto* chain = host.hooks_.permissions;
    LuaArgs lua{plugin.sandbox->state()};

    const auto asked = [server](LuaArgs& args, std::int32_t index) {
        Asked who;
        if (args.isTable(index)) {
            args.pushField(index, "is_console");
            who.console = args.isBoolean(-1) && args.toBoolean(-1);
            args.pop(1);
            who.name = args.fieldAsText(index, "name").value_or("");
        } else {
            who.name = args.checkString(index);
        }
        if (!who.console)
            if (const auto* found = server->findPlayer(who.name)) {
                who.uuid = found->uuid;
                who.server = found->server;
            }
        if (args.isTable(index + 2)) {
            who.server = args.fieldAsText(index + 2, "server").value_or(who.server);
            who.world = args.fieldAsText(index + 2, "world").value_or("");
        }
        return who;
    };
    const auto subjectOf = [](const Asked& who) {
        return who.console ? Subject::console() : Subject::player(who.uuid, who.name);
    };

    lua.pushTable();
    wire::lua::pushHostFunction(lua, "permissions.check", [chain, asked, subjectOf](LuaArgs& args) {
        const Asked who = asked(args, 1);
        const std::string node = args.checkString(2);
        const Tristate answer =
            chain ? chain->check(subjectOf(who), node, Context{who.server, who.world, {}}) : Tristate::kUndefined;
        if (answer == Tristate::kUndefined)
            args.pushNil();
        else
            args.pushBoolean(answer == Tristate::kTrue);
        return 1;
    });
    lua.setField("check");

    wire::lua::pushHostFunction(lua, "permissions.meta", [chain, asked, subjectOf](LuaArgs& args) {
        const Asked who = asked(args, 1);
        const std::string key = args.checkString(2);
        const auto value = chain ? chain->meta(subjectOf(who), key, Context{who.server, who.world, {}}) : std::nullopt;
        if (value)
            args.pushString(*value);
        else
            args.pushNil();
        return 1;
    });
    lua.setField("meta");

    wire::lua::pushHostFunction(lua, "permissions.changed", [chain](LuaArgs&) {
        if (chain)
            chain->changed();
        return 0;
    });
    lua.setField("changed");

    wire::lua::pushHostFunction(lua, "permissions.provide", [server, self, chain](LuaArgs& args) {
        if (!chain)
            return args.fail("permissions are not available on this proxy");
        auto slot = std::make_shared<PluginHost::PermissionSlot>();
        slot->plugin = self;
        std::int32_t priority = wire::permissions::kPriorityPlugin;
        if (args.isFunction(1)) {
            slot->check = LuaRef::of(args.state(), 1);
        } else if (args.isTable(1)) {
            args.pushField(1, "check");
            if (args.isFunction(-1))
                slot->check = LuaRef::of(args.state(), -1);
            args.pop(1);
            args.pushField(1, "meta");
            if (args.isFunction(-1))
                slot->meta = LuaRef::of(args.state(), -1);
            args.pop(1);
        }
        if (!slot->check.valid() && !slot->meta.valid())
            return args.fail("permissions.provide(fn) needs a function or a table with check and meta");
        if (args.isTable(2)) {
            args.pushField(2, "priority");
            if (args.isInteger(-1))
                priority = static_cast<std::int32_t>(args.toInteger(-1));
            args.pop(1);
            if (priority <= wire::permissions::kPriorityDefault)
                return args.fail("a plugin provider's priority must be above " +
                                 std::to_string(wire::permissions::kPriorityDefault));
        }
        wire::permissions::Provider provider{.id = "lua:" + self->id, .priority = priority, .check = {}, .meta = {}};
        const std::weak_ptr<PluginHost::PermissionSlot> weak = slot;
        if (slot->check.valid())
            provider.check = [server, weak](const wire::permissions::Query& query) {
                const auto held = weak.lock();
                Tristate answer = Tristate::kUndefined;
                if (held)
                    static_cast<void>(
                        server->callProvider(*held, false, query.subject(), query.node(), query.context(), answer));
                return answer;
            };
        if (slot->meta.valid())
            provider.meta = [server, weak](const Subject& subject, std::string_view key,
                                           const Context& context) -> std::optional<std::string> {
                const auto held = weak.lock();
                Tristate ignored = Tristate::kUndefined;
                return held ? server->callProvider(*held, true, subject, key, context, ignored) : std::nullopt;
            };
        slot->handle = chain->provide(std::move(provider));
        self->permissionSlots.slots.push_back(slot);
        args.pushTable();
        wire::lua::pushHostFunction(args, "permissions.remove", [self, weak](LuaArgs&) {
            if (const auto held = weak.lock()) {
                held->retire();
                std::erase(self->permissionSlots.slots, held);
            }
            return 0;
        });
        args.setField("remove");
        return 1;
    });
    lua.setField("provide");
    lua.setGlobal("permissions");
}

} // namespace basalt::proxy::plugins
