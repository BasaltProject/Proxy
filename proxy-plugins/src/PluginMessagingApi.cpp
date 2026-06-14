#include "PluginInternal.hpp"

#include <basalt/proxy/plugins/BungeeMessaging.hpp>

#include <algorithm>

namespace basalt::proxy::plugins {
namespace {

constexpr std::size_t kMaxChannel = 128;
constexpr std::size_t kMaxChannelsPerPlugin = 64;

[[nodiscard]] bool validChannel(std::string_view channel) {
    return !channel.empty() && channel.size() <= kMaxChannel &&
           std::ranges::all_of(channel, [](unsigned char c) { return c > 32 && c < 127; });
}

} // namespace

void PluginApi::installMessaging(PluginHost& host, PluginHost::Plugin& plugin) {
    using wire::lua::LuaArgs;
    LuaArgs lua{plugin.sandbox->state()};
    PluginHost* server = &host;
    PluginHost::Plugin* self = &plugin;

    lua.pushTable();
    wire::lua::pushHostFunction(lua, "channels.register", [server, self](LuaArgs& args) {
        const std::string channel = args.checkString(1);
        if (!validChannel(channel))
            return args.fail("a channel is 1 to 128 printable characters, as in 'namespace:name'");
        if (isBungeeChannel(channel))
            return args.fail("'" + channel + "' is answered by the proxy itself");
        if (self->channels.size() >= kMaxChannelsPerPlugin)
            return args.fail("a plugin may register at most 64 channels");
        if (std::ranges::find(self->channels, channel) == self->channels.end())
            self->channels.push_back(channel);
        std::unique_lock lock{server->channelsMutex_};
        server->channels_.insert(channel);
        return 0;
    });
    lua.setField("register");
    wire::lua::pushHostFunction(lua, "channels.unregister", [server, self](LuaArgs& args) {
        const std::string channel = args.checkString(1);
        std::erase(self->channels, channel);
        const bool stillWanted = std::ranges::any_of(server->plugins_, [&](const auto& other) {
            return std::ranges::find(other->channels, channel) != other->channels.end();
        });
        if (!stillWanted) {
            std::unique_lock lock{server->channelsMutex_};
            server->channels_.erase(channel);
        }
        return 0;
    });
    lua.setField("unregister");
    wire::lua::pushHostFunction(lua, "channels.send", [server](LuaArgs& args) {
        const std::string player = args.checkString(1);
        const std::string channel = args.checkString(2);
        const std::string data = args.checkString(3);
        const bool toClient = args.stringOrNull(4).value_or("backend") == "client";
        if (!validChannel(channel))
            return args.fail("a channel is 1 to 128 printable characters, as in 'namespace:name'");
        const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(data.data()), data.size()};
        args.pushBoolean(server->hooks_.sendMessage && server->hooks_.sendMessage(player, channel, bytes, !toClient));
        return 1;
    });
    lua.setField("send");
    lua.setGlobal("channels");
}

} // namespace basalt::proxy::plugins
