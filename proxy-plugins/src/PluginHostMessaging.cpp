#include "PluginHostSupport.hpp"

namespace basalt::proxy::plugins {
std::vector<std::string> PluginHost::names() {
    std::lock_guard serial{reloadMutex_};
    std::vector<std::string> result;
    for (const auto& [id, installed] : installed_)
        result.push_back(installed.second.name.empty() ? id : installed.second.name);
    const std::lock_guard names{foreignMutex_};
    result.insert(result.end(), foreignNames_.begin(), foreignNames_.end());
    return result;
}

std::vector<std::string> PluginHost::playersOn(std::string_view server) const {
    std::vector<std::string> names;
    for (const auto& [name, player] : roster_)
        if (server.empty() || player.server == server)
            names.push_back(player.name);
    return names;
}

std::vector<std::string> PluginHost::channelNames() const {
    std::set<std::string, std::less<>> names;
    {
        std::shared_lock lock{channelsMutex_};
        names.insert(channels_.begin(), channels_.end());
    }
    for (const auto& adapter : adapters_)
        for (auto& name : adapter->channels())
            names.insert(std::move(name));
    return {names.begin(), names.end()};
}

bool PluginHost::message(std::string_view player, std::string_view channel, std::span<const std::uint8_t> data,
                         bool fromBackend) {
    if (!isBungeeChannel(channel)) {
        std::shared_lock lock{channelsMutex_};
        const bool foreign =
            std::ranges::any_of(adapters_, [&](const auto& adapter) { return adapter->listens(channel); });
        if (!foreign && !channels_.contains(channel))
            return false;
    }
    static_cast<void>(enqueue(MessageCall{std::string{player}, std::string{channel},
                                          std::vector<std::uint8_t>(data.begin(), data.end()), fromBackend}));
    return true;
}

void PluginHost::deliverMessage(const MessageCall& call) {
    for (auto& adapter : adapters_)
        if (adapter->message(call.player, call.channel, call.data, call.fromBackend))
            return;
    if (isBungeeChannel(call.channel) && call.fromBackend) {
        BungeeContext context;
        context.player = [this](std::string_view name) -> std::optional<BungeePlayer> {
            const Player* found = findPlayer(name);
            if (found == nullptr)
                return std::nullopt;
            return BungeePlayer{found->name, found->uuid, found->server, found->address, found->port};
        };
        context.playersOn = [this](std::string_view server) { return playersOn(server); };
        context.serverExists = [this](std::string_view server) {
            return hooks_.servers && std::ranges::contains(hooks_.servers(), server);
        };
        context.serverAddress = [this](std::string_view server) {
            return hooks_.serverAddress ? hooks_.serverAddress(server) : std::nullopt;
        };
        context.connect = [this](std::string_view player, std::string_view server) {
            return hooks_.send && hooks_.send(player, server);
        };
        context.kick = [this](std::string_view player, std::string_view reason) {
            if (hooks_.kick)
                hooks_.kick(player, reason);
        };
        context.tell = [this](std::string_view player, std::string_view text) {
            if (hooks_.tell)
                hooks_.tell(player, text);
        };
        context.tellRaw = [this](std::string_view player, std::string_view json) {
            if (const auto component = componentOfJson(json); component && hooks_.tellComponent)
                hooks_.tellComponent(player, *component);
        };
        context.kickRaw = [this](std::string_view player, std::string_view json) {
            if (const auto component = componentOfJson(json); component && hooks_.kickComponent)
                hooks_.kickComponent(player, *component);
        };
        context.toBackend = [this](std::string_view player, std::string_view channel,
                                   std::span<const std::uint8_t> data) {
            if (hooks_.sendMessage)
                static_cast<void>(hooks_.sendMessage(player, channel, data, true));
        };
        if (!handleBungeeMessage(context, call.player, call.data) && hooks_.log)
            hooks_.log("bungeecord", "ignored a message from " + call.player + " that names no known subchannel");
    }
    const Player* known = findPlayer(call.player);
    const Player subject = known ? *known : Player{call.player, {}, {}};
    for (auto& plugin : plugins_) {
        if (plugin->broken)
            continue;
        const auto found = plugin->handlers.find(std::string{kMessageEvent});
        if (found == plugin->handlers.end())
            continue;
        for (const Plugin::Handler& handler : found->second) {
            const auto wanted = handler.filters.find("channel");
            if (wanted != handler.filters.end() && wanted->second != call.channel)
                continue;
            this->call(
                *plugin, handler.function,
                [&](wire::lua::LuaArgs& lua) {
                    lua.pushTable();
                    PluginApi::pushPlayer(lua, *plugin, subject);
                    lua.setField("player");
                    lua.pushString(call.channel);
                    lua.setField("channel");
                    lua.pushString(std::string{reinterpret_cast<const char*>(call.data.data()), call.data.size()});
                    lua.setField("data");
                    lua.pushString(call.fromBackend ? "backend" : "client");
                    lua.setField("source");
                    return 1;
                },
                std::string{kMessageEvent});
        }
    }
}

const PluginHost::Player* PluginHost::findPlayer(std::string_view name) const {
    const auto exact = roster_.find(name);
    if (exact != roster_.end())
        return &exact->second;
    for (const auto& [key, player] : roster_)
        if (sameName(key, name))
            return &player;
    return nullptr;
}

bool PluginHost::allowed(const Player& player, const std::optional<std::string>& permission) const {
    if (!permission)
        return true;
    return hooks_.hasPermission && hooks_.hasPermission(player.name, *permission);
}

} // namespace basalt::proxy::plugins
