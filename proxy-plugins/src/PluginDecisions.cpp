#include "PluginInternal.hpp"

#include <algorithm>

namespace basalt::proxy::plugins {
namespace {

using Kind = runtime::DecisionKind;

struct KindInfo {
    std::string_view event;
    unsigned bit;
};

KindInfo infoOf(Kind kind) {
    switch (kind) {
    case Kind::kLogin:
        return {kLoginEvent, 1U};
    case Kind::kPreConnect:
        return {kPreConnectEvent, 2U};
    case Kind::kKick:
        return {kKickEvent, 4U};
    case Kind::kChat:
        return {kChatEvent, 8U};
    case Kind::kCommand:
        return {kCommandEvent, 16U};
    case Kind::kPing:
        return {kPingEvent, 32U};
    }
    return {kLoginEvent, 1U};
}

} // namespace

unsigned decisionBit(std::string_view event) {
    for (const Kind kind : {Kind::kLogin, Kind::kPreConnect, Kind::kKick, Kind::kChat, Kind::kCommand, Kind::kPing})
        if (infoOf(kind).event == event)
            return infoOf(kind).bit;
    return 0;
}

unsigned decisionBit(runtime::DecisionKind kind) { return infoOf(kind).bit; }

struct PluginHost::DecisionRun {
    struct Step {
        Plugin* plugin;
        const Plugin::Handler* handler;
    };

    runtime::DecisionRequest request;
    std::chrono::steady_clock::time_point received;
    std::vector<Step> steps;
    Player subject;
    bool cancelled = false;
    std::string reason;
    std::string server;
    std::string text;
    std::int64_t maxPlayers = -1;
    std::int64_t online = -1;
};

bool PluginHost::decide(runtime::DecisionRequest request) {
    if ((deciders_.load() & infoOf(request.kind).bit) == 0)
        return false;
    return enqueue(DecisionCall{std::move(request), std::chrono::steady_clock::now()});
}

void PluginHost::deliverDecision(const DecisionCall& call) {
    const std::string name{infoOf(call.request.kind).event};
    auto run = std::make_shared<DecisionRun>();
    run->request = call.request;
    run->received = call.received;
    const Kind kind = call.request.kind;
    if (kind == Kind::kPreConnect)
        run->server = call.request.backend;
    if (kind == Kind::kChat || kind == Kind::kCommand || kind == Kind::kPing)
        run->text = call.request.text;
    if (kind == Kind::kKick)
        run->reason = call.request.text;
    const Player* known = findPlayer(call.request.player);
    run->subject = known ? *known
                         : Player{call.request.player, call.request.uuid, call.request.from, call.request.address,
                                  call.request.port};
    for (auto& plugin : plugins_) {
        if (plugin->broken)
            continue;
        const auto found = plugin->handlers.find(name);
        if (found == plugin->handlers.end())
            continue;
        for (const Plugin::Handler& handler : found->second)
            if (!handler.observer)
                run->steps.push_back({plugin.get(), &handler});
    }
    std::ranges::stable_sort(run->steps, {}, [](const DecisionRun::Step& step) { return step.handler->priority; });
    stepDecision(run, 0);
}

void PluginHost::stepDecision(const std::shared_ptr<DecisionRun>& run, std::size_t index) {
    const Kind kind = run->request.kind;
    if (index >= run->steps.size()) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - run->received);
        if (elapsed.count() >= run->request.deadlineMs) {
            if (hooks_.log)
                hooks_.log("decisions", "a decision for " + run->request.player + " took " +
                                            std::to_string(elapsed.count()) + " ms; the proxy already went on");
            return;
        }
        for (auto& adapter : adapters_) {
            if (!adapter->decides(kind))
                continue;
            runtime::DecisionRequest asked = run->request;
            if (kind == Kind::kPreConnect)
                asked.backend = run->server;
            else if (kind == Kind::kChat || kind == Kind::kCommand)
                asked.text = run->text;
            else if (kind == Kind::kKick)
                asked.text = run->reason;
            const auto answer = adapter->decide(asked);
            if (!answer)
                continue;
            if (answer->deny) {
                run->cancelled = true;
                run->reason = answer->reason;
            }
            if (!answer->redirect.empty())
                run->server = answer->redirect;
            if (!answer->text.empty())
                (kind == Kind::kKick ? run->reason : run->text) = answer->text;
            if (answer->maxPlayers)
                run->maxPlayers = *answer->maxPlayers;
            if (answer->online)
                run->online = *answer->online;
        }
        if (hooks_.resolveDecision) {
            runtime::DecisionResult result;
            result.deny = run->cancelled;
            result.reason = run->reason;
            if (kind == Kind::kPreConnect && !run->cancelled && run->server != run->request.backend)
                result.redirect = run->server;
            if (kind == Kind::kKick) {
                result.deny = false;
                result.redirect = run->server;
                result.reason.clear();
                if (run->reason != run->request.text)
                    result.text = run->reason;
            } else if (kind == Kind::kCommand) {
                if (run->text != run->request.text)
                    result.text = run->text;
            } else if (kind == Kind::kPing) {
                result.text = run->text == run->request.text ? std::string{} : run->text;
                if (run->maxPlayers >= 0)
                    result.maxPlayers = static_cast<std::int32_t>(run->maxPlayers);
                if (run->online >= 0)
                    result.online = static_cast<std::int32_t>(run->online);
            }
            hooks_.resolveDecision(run->request.player, run->request.token, std::move(result));
        }
        return;
    }
    const auto step = run->steps[index];
    if (step.plugin->broken) {
        stepDecision(run, index + 1);
        return;
    }
    const auto table = std::make_shared<wire::lua::LuaRef>();
    Plugin* plugin = step.plugin;
    call(
        *plugin, step.handler->function,
        [&, table](wire::lua::LuaArgs& lua) {
            lua.pushTable();
            const auto text = [&](std::string_view key, const std::string& value) {
                lua.pushString(value);
                lua.setField(key);
            };
            if (kind != Kind::kPing) {
                PluginApi::pushPlayer(lua, *plugin, run->subject);
                lua.setField("player");
                text("player_name", run->request.player);
                text("player_uuid", run->request.uuid);
                text("address", run->request.address);
                lua.pushInteger(run->request.port);
                lua.setField("port");
            }
            text("host", run->request.host);
            if (kind == Kind::kPreConnect || kind == Kind::kKick) {
                text("server", run->server);
                text("from_server", run->request.from);
            }
            if (kind == Kind::kChat)
                text("message", run->text);
            if (kind == Kind::kCommand)
                text("command", run->text);
            if (kind == Kind::kKick)
                text("reason", run->reason);
            if (kind == Kind::kPing) {
                text("protocol", run->request.text);
                text("motd", run->text == run->request.text ? std::string{} : run->text);
                lua.pushInteger(-1);
                lua.setField("max_players");
                lua.pushInteger(-1);
                lua.setField("online");
            }
            if (kind != Kind::kKick && kind != Kind::kPing) {
                lua.pushBoolean(run->cancelled);
                lua.setField("cancelled");
                text("reason", run->reason);
            }
            *table = wire::lua::LuaRef::of(lua.state(), -1);
            return 1;
        },
        infoOf(kind).event,
        [this, run, index, plugin, table, kind](bool ok) {
            if (ok) {
                wire::lua::LuaArgs lua{plugin->sandbox->state()};
                table->push(lua);
                const auto readText = [&](std::string_view key, std::string& into) {
                    lua.pushField(-1, key);
                    if (lua.isString(-1))
                        into = lua.toText(-1);
                    lua.pop(1);
                };
                const auto readNumber = [&](std::string_view key, std::int64_t& into) {
                    lua.pushField(-1, key);
                    if (lua.isNumber(-1))
                        into = lua.toInteger(-1);
                    lua.pop(1);
                };
                if (kind != Kind::kKick && kind != Kind::kPing) {
                    lua.pushField(-1, "cancelled");
                    run->cancelled = lua.toBoolean(-1);
                    lua.pop(1);
                    readText("reason", run->reason);
                }
                if (kind == Kind::kPreConnect || kind == Kind::kKick)
                    readText("server", run->server);
                if (kind == Kind::kKick)
                    readText("reason", run->reason);
                if (kind == Kind::kChat)
                    readText("message", run->text);
                if (kind == Kind::kCommand)
                    readText("command", run->text);
                if (kind == Kind::kPing) {
                    readText("motd", run->text);
                    readNumber("max_players", run->maxPlayers);
                    readNumber("online", run->online);
                }
                lua.pop(1);
            }
            stepDecision(run, index + 1);
        });
}

} // namespace basalt::proxy::plugins
