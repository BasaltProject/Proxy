#include "PluginHostSupport.hpp"

namespace basalt::proxy::plugins {
void PluginHost::run(std::stop_token stop) {
    {
        std::vector<std::string> foreign;
        for (const auto& adapter : adapters_)
            for (const ForeignPlugin& plugin : adapter->plugins())
                foreign.push_back(plugin.name + " (" + std::string{adapter->label()} + ")");
        const std::lock_guard names{foreignMutex_};
        foreignNames_ = std::move(foreign);
    }
    // Enable runs here rather than while loading so a handler can wait, schedule and look at the players.
    for (auto& plugin : plugins_) {
        for (const wire::lua::LuaRef& handler : plugin->onEnable) {
            const std::lock_guard<std::recursive_timed_mutex> turn{luaMutex_};
            if (plugin->broken)
                break;
            call(*plugin, handler, {}, "plugin.enable");
        }
    }
    while (!stop.stop_requested()) {
        Task task;
        bool have = false;
        {
            std::unique_lock lock{mutex_};
            std::optional<std::chrono::steady_clock::time_point> next;
            for (const auto& timer : timers_)
                if (!next || timer->due < *next)
                    next = timer->due;
            const auto ready = [this] { return !queue_.empty(); };
            if (next)
                wake_.wait_until(lock, stop, *next, ready);
            else
                wake_.wait(lock, stop, ready);
            if (stop.stop_requested())
                return;
            if (!queue_.empty()) {
                task = std::move(queue_.front());
                queue_.pop_front();
                have = true;
            }
        }
        const std::lock_guard<std::recursive_timed_mutex> turn{luaMutex_};
        runTimers(std::chrono::steady_clock::now());
        if (!have)
            continue;
        if (const auto* event = std::get_if<runtime::PlayerEvent>(&task))
            deliver(*event);
        else if (const auto* console = std::get_if<ConsoleCall>(&task))
            invoke(console->words, std::nullopt);
        else if (const auto* sent = std::get_if<MessageCall>(&task))
            deliverMessage(*sent);
        else if (const auto* asked = std::get_if<DecisionCall>(&task))
            deliverDecision(*asked);
        else {
            const auto& typed = std::get<PlayerCommandCall>(task);
            const Player* known = findPlayer(typed.player);
            invoke(typed.words, known ? *known : Player{typed.player, {}, {}});
        }
        delivered_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PluginHost::runTimers(std::chrono::steady_clock::time_point now) {
    // A timer may register another, so due ones are taken out before any of them runs.
    std::vector<std::unique_ptr<Timer>> due;
    {
        std::lock_guard lock{mutex_};
        for (auto it = timers_.begin(); it != timers_.end();) {
            if ((*it)->cancelled && (*it)->cancelled->load()) {
                it = timers_.erase(it);
            } else if ((*it)->due <= now) {
                due.push_back(std::move(*it));
                it = timers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& timer : due) {
        if (timer->plugin->broken)
            continue;
        if (timer->resume) {
            timer->plugin->sandbox->refill();
            timer->resume();
            continue;
        }
        call(*timer->plugin, *timer->function, timer->arguments, "timer");
        if (timer->period.count() > 0 && !(timer->cancelled && timer->cancelled->load())) {
            timer->due = now + timer->period;
            std::lock_guard lock{mutex_};
            timers_.push_back(std::move(timer));
        }
    }
}

void PluginHost::call(Plugin& plugin, const wire::lua::LuaRef& function,
                      const std::function<std::int32_t(wire::lua::LuaArgs&)>& pushArguments, std::string_view why,
                      std::function<void(bool)> after) {
    plugin.sandbox->refill();
    wire::lua::LuaArgs lua{plugin.sandbox->state()};
    function.push(lua);
    const std::int32_t argumentCount = pushArguments ? pushArguments(lua) : 0;
    const std::string reason{why};
    Plugin* target = &plugin;
    static_cast<void>(wire::lua::LuaCall::start(
        *plugin.sandbox, argumentCount,
        [this, target, reason, after = std::move(after)](std::optional<wire::lua::LuaFailure> failure) {
            if (!failure)
                target->strikes = 0;
            else
                report(*target, *failure, reason);
            if (after)
                after(!failure);
        }));
}

bool PluginHost::deliverPacket(const void* owner, const wire::lua::LuaRef& function,
                               wire::plugins::PluginPacket& packet) {
    std::unique_lock<std::recursive_timed_mutex> turn{luaMutex_, std::defer_lock};
    if (!turn.try_lock_for(settings_.packetTimeout)) {
        const std::size_t waited = packetTimeouts_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hooks_.log && (waited == 1 || waited % 1000 == 0))
            hooks_.log("plugins", "packet listeners waited over " + std::to_string(settings_.packetTimeout.count()) +
                                      " ms for the plugin interpreter: " + std::to_string(waited) +
                                      " packet(s) so far went on untouched or were cancelled by fail = \"closed\" "
                                      "listeners");
        return false;
    }
    auto& plugin = *static_cast<Plugin*>(const_cast<void*>(owner));
    if (plugin.broken)
        return true;
    wire::plugins::PacketViewHost view;
    view.maxBodyBytes = settings_.packetMaxBytes;
    view.pushPlayer = [&plugin, this](wire::lua::LuaArgs& lua, const wire::plugins::PluginPacket& source) {
        if (source.playerName.empty()) {
            lua.pushNil();
            return;
        }
        const Player* known = findPlayer(source.playerName);
        PluginApi::pushPlayer(lua, plugin,
                              known ? *known : Player{source.playerName, source.playerUuid, source.backend});
    };
    plugin.sandbox->refill();
    std::shared_ptr<wire::plugins::PacketViewLease> lease;
    const auto answered = wire::lua::callSync(
        *plugin.sandbox, function,
        [&](wire::lua::LuaArgs& lua) {
            lease = wire::plugins::pushPacketView(lua, packet, view);
            return 1;
        },
        0);
    if (lease)
        lease->close();
    if (answered)
        plugin.strikes = 0;
    else
        report(plugin, answered.error(), "packet " + packet.name);
    return true;
}

void PluginHost::report(Plugin& plugin, const wire::lua::LuaFailure& failure, std::string_view why) {
    ++plugin.strikes;
    if (hooks_.log)
        hooks_.log(plugin.id, std::string{why} + " failed: " + failure.message);
    if (failure.outOfBudget() || failure.outOfMemory() || plugin.strikes >= kStrikeLimit) {
        plugin.broken = true;
        if (hooks_.log)
            hooks_.log(plugin.id, "disabled until the next reload after repeated failures");
    }
}

void PluginHost::callEvent(Plugin& plugin, const std::string& name, const Player* subject, const std::string& from) {
    const auto found = plugin.handlers.find(name);
    if (found == plugin.handlers.end())
        return;
    for (const Plugin::Handler& handler : found->second) {
        if (plugin.broken)
            return;
        if (!handler.observer && (name == kChatEvent || name == kKickEvent))
            continue;
        if (subject != nullptr) {
            const auto matches = [&](std::string_view key, const std::string& value) {
                const auto wanted = handler.filters.find(key);
                return wanted == handler.filters.end() || sameName(wanted->second, value);
            };
            if (!matches("player_name", subject->name) || !matches("player_uuid", subject->uuid) ||
                !matches("server", subject->server) ||
                (name != kChatEvent && name != kKickEvent && !matches("from_server", from)))
                continue;
        }
        call(
            plugin, handler.function,
            [&](wire::lua::LuaArgs& lua) {
                lua.pushTable();
                if (subject != nullptr) {
                    PluginApi::pushPlayer(lua, plugin, *subject);
                    lua.setField("player");
                    lua.pushString(subject->name);
                    lua.setField("player_name");
                    lua.pushString(subject->uuid);
                    lua.setField("player_uuid");
                    lua.pushString(subject->server);
                    lua.setField("server");
                    if (!from.empty()) {
                        lua.pushString(from);
                        lua.setField(name == kChatEvent ? "message" : name == kKickEvent ? "reason" : "from_server");
                    }
                }
                return 1;
            },
            name);
    }
}

void PluginHost::callPing(Plugin& plugin, const std::string& host, const std::string& protocol) {
    const auto found = plugin.handlers.find(std::string{kPingEvent});
    if (found == plugin.handlers.end())
        return;
    for (const Plugin::Handler& handler : found->second) {
        if (plugin.broken)
            return;
        if (!handler.observer)
            continue;
        call(
            plugin, handler.function,
            [&](wire::lua::LuaArgs& lua) {
                lua.pushTable();
                lua.pushString(host);
                lua.setField("host");
                lua.pushInteger(std::stoll(protocol));
                lua.setField("protocol");
                return 1;
            },
            std::string{kPingEvent});
    }
}

void PluginHost::callLogin(Plugin& plugin, const std::string& player, const std::string& uuid) {
    const auto found = plugin.handlers.find(std::string{kLoginEvent});
    if (found == plugin.handlers.end())
        return;
    for (const Plugin::Handler& handler : found->second) {
        if (plugin.broken)
            return;
        if (!handler.observer)
            continue;
        call(
            plugin, handler.function,
            [&](wire::lua::LuaArgs& lua) {
                lua.pushTable();
                lua.pushString(player);
                lua.setField("player_name");
                lua.pushString(uuid);
                lua.setField("player_uuid");
                return 1;
            },
            std::string{kLoginEvent});
    }
}

void PluginHost::callAntibot(Plugin& plugin, const runtime::PlayerEvent& event) {
    const auto found = plugin.handlers.find(std::string{kAntibotEvent});
    if (found == plugin.handlers.end())
        return;
    for (const Plugin::Handler& handler : found->second) {
        if (plugin.broken)
            return;
        call(
            plugin, handler.function,
            [&](wire::lua::LuaArgs& lua) {
                lua.pushTable();
                lua.pushString(event.backend);
                lua.setField("kind");
                lua.pushString(event.player);
                lua.setField("player_name");
                const auto first = event.uuid.find(':');
                const auto second = event.uuid.find(':', first + 1);
                lua.pushString(event.uuid.substr(0, first));
                lua.setField("action");
                lua.pushString(first == std::string::npos ? std::string{}
                                                          : event.uuid.substr(first + 1, second - first - 1));
                lua.setField("reason");
                lua.pushInteger(second == std::string::npos ? 0 : std::stoll(event.uuid.substr(second + 1)));
                lua.setField("skipped");
                lua.pushString(event.from);
                lua.setField("origin");
                return 1;
            },
            std::string{kAntibotEvent});
    }
}

void PluginHost::deliver(const runtime::PlayerEvent& event) {
    using Kind = runtime::PlayerEventKind;
    for (auto& adapter : adapters_)
        adapter->notify(event);
    if (event.kind == Kind::kLatency) {
        if (const auto known = roster_.find(event.player); known != roster_.end())
            known->second.ping = event.value;
        return;
    }
    Player subject{event.player, event.uuid, event.backend, event.address, event.port};
    if (const auto known = roster_.find(event.player); known != roster_.end() && event.address.empty()) {
        subject.address = known->second.address;
        subject.port = known->second.port;
        subject.ping = known->second.ping;
    }
    std::string name;
    if (event.kind == Kind::kJoin) {
        name = std::string{kJoinEvent};
        roster_[event.player] = subject;
    } else if (event.kind == Kind::kSwitch) {
        name = std::string{kServerChangeEvent};
        roster_[event.player] = subject;
    } else if (event.kind == Kind::kLogin) {
        for (auto& plugin : plugins_)
            if (!plugin->broken)
                callLogin(*plugin, event.player, event.uuid);
        return;
    } else if (event.kind == Kind::kAntibot) {
        for (auto& plugin : plugins_)
            if (!plugin->broken)
                callAntibot(*plugin, event);
        return;
    } else if (event.kind == Kind::kPing) {
        for (auto& plugin : plugins_)
            if (!plugin->broken)
                callPing(*plugin, event.backend, event.from);
        return;
    } else if (event.kind == Kind::kChat || event.kind == Kind::kKick) {
        name = std::string{event.kind == Kind::kChat ? kChatEvent : kKickEvent};
        if (const auto known = roster_.find(event.player); known != roster_.end())
            subject = known->second;
    } else {
        name = std::string{kQuitEvent};
    }
    for (auto& plugin : plugins_)
        if (!plugin->broken)
            callEvent(*plugin, name, &subject, event.from);
    if (event.kind == Kind::kLeave)
        roster_.erase(event.player);
}

void PluginHost::listPlugins() {
    std::string line;
    const auto add = [&line](std::string_view label, std::string_view name, std::string_view version) {
        line += (line.empty() ? "" : ", ") + std::string{"["} + std::string{label} + "] " + std::string{name} + " " +
                std::string{version};
    };
    for (const auto& plugin : plugins_)
        add("Lua", plugin->name, plugin->version);
    for (const auto& adapter : adapters_)
        for (const ForeignPlugin& plugin : adapter->plugins())
            add(adapter->label(), plugin.name, plugin.version);
    if (hooks_.log)
        hooks_.log("plugins", line.empty() ? "No plugins loaded." : "Plugins: " + line);
}

void PluginHost::invoke(const std::vector<std::string>& words, const std::optional<Player>& source) {
    const auto roots = commands_.find(words.front());
    if (roots == commands_.end()) {
        if (!source && words.front() == "plugins")
            return listPlugins();
        if (!source && foreignCommands_.contains(words.front())) {
            std::string line;
            for (const std::string& word : words)
                line += (line.empty() ? "" : " ") + word;
            for (auto& adapter : adapters_)
                if (adapter->console(line))
                    return;
        }
        return;
    }
    const Entry* best = nullptr;
    for (const auto& entry : roots->second) {
        const auto& literals = entry->spec.literals;
        if (literals.size() > words.size() || entry->plugin->broken)
            continue;
        if (!std::equal(literals.begin(), literals.end(), words.begin()))
            continue;
        if (best == nullptr || literals.size() > best->spec.literals.size())
            best = entry.get();
    }
    if (best == nullptr)
        return;
    Plugin& plugin = *best->plugin;
    const auto answer = [&](std::string_view text) {
        if (source && hooks_.tell)
            hooks_.tell(source->name, text);
        else if (hooks_.log)
            hooks_.log(plugin.id, text);
    };
    if (source && !allowed(*source, best->spec.permission)) {
        answer("<red>You do not have permission to use this command.");
        return;
    }

    const auto values = wire::plugins::parseCommandWords(best->spec, words);
    if (!values) {
        answer("<red>Usage: /" + best->spec.usage());
        return;
    }
    call(
        plugin, best->function,
        [&](wire::lua::LuaArgs& lua) {
            if (source) {
                PluginApi::pushPlayer(lua, plugin, *source);
            } else {
                lua.pushTable();
                lua.pushBoolean(true);
                lua.setField("is_console");
                lua.pushString("CONSOLE");
                lua.setField("name");
                plugin.consoleMeta.push(lua);
                lua.setMetatable(-2);
            }
            for (const wire::plugins::CommandValue& value : *values)
                wire::plugins::pushCommandValue(lua, value);
            return static_cast<std::int32_t>(values->size() + 1);
        },
        "command " + best->spec.name());
}

} // namespace basalt::proxy::plugins
