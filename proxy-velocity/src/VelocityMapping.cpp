#include <basalt/proxy/velocity/VelocityMapping.hpp>

#include <array>

namespace basalt::proxy::velocity {
namespace {

using plugins::MappingStatus;

constexpr std::array kTable = std::to_array<plugins::AdapterMapping>({
    {"PreLoginEvent", "DecisionKind::kLogin", "on('player.login') with e.cancelled, e.reason", MappingStatus::kNative},
    {"LoginEvent", "DecisionKind::kLogin", "on('player.login')", MappingStatus::kNative},
    {"PostLoginEvent", "PlayerEventKind::kJoin", "on('player.join')", MappingStatus::kNative},
    {"PlayerChooseInitialServerEvent", "DecisionKind::kPreConnect (first route)", "on('player.server_pre_connect')",
     MappingStatus::kNative},
    {"ServerPreConnectEvent", "DecisionKind::kPreConnect", "on('player.server_pre_connect') with e.server, e.cancelled",
     MappingStatus::kNative},
    {"ServerConnectedEvent", "PlayerEventKind::kSwitch", "on('player.server_change')", MappingStatus::kPartial},
    {"ServerPostConnectEvent", "PlayerEventKind::kSwitch", "on('player.server_change')", MappingStatus::kPartial},
    {"KickedFromServerEvent", "DecisionKind::kKick", "on('player.kick') with e.server, e.reason",
     MappingStatus::kNative},
    {"DisconnectEvent", "PlayerEventKind::kLeave", "on('player.quit')", MappingStatus::kNative},
    {"PlayerChatEvent", "DecisionKind::kChat", "on('player.chat') with e.cancelled", MappingStatus::kPartial},
    {"CommandExecuteEvent", "DecisionKind::kCommand", "on('player.command') with e.command, e.cancelled",
     MappingStatus::kNative},
    {"ProxyPingEvent", "DecisionKind::kPing", "on('server.ping') with e.motd, e.online, e.max_players",
     MappingStatus::kNative},
    {"PluginMessageEvent", "PluginHost::message", "on('player.message')", MappingStatus::kPartial},
    {"Player.getUsername/getUniqueId", "PluginHost::Player", "player.name, player.uuid", MappingStatus::kNative},
    {"Player.getRemoteAddress", "PluginHost::Player", "player.address, player.port", MappingStatus::kNative},
    {"Player.getCurrentServer", "PluginHost::Player", "player.server", MappingStatus::kNative},
    {"Player.createConnectionRequest", "hooks.send", "player:connect(server)", MappingStatus::kPartial},
    {"Player.disconnect", "hooks.kick", "player:kick(reason)", MappingStatus::kNative},
    {"Player.sendMessage", "hooks.tell", "player:send(text)", MappingStatus::kNative},
    {"Player.sendPluginMessage", "hooks.sendMessage", "player:message(channel, data)", MappingStatus::kNative},
    {"Player.getPing", "ClientSession keepalive RTT", "player.ping", MappingStatus::kNative},
    {"ProxyServer.getAllPlayers/getPlayerCount", "PluginHost::roster_", "players.online(), players.count()",
     MappingStatus::kNative},
    {"ProxyServer.getAllServers", "hooks.servers", "servers.list()", MappingStatus::kNative},
    {"RegisteredServer.getServerInfo().getAddress", "hooks.serverAddress", "servers.address(name)",
     MappingStatus::kNative},
    {"RegisteredServer.getPlayersConnected", "PluginHost::playersOn", "servers.players(name)", MappingStatus::kNative},
    {"ChannelRegistrar.register", "PluginHost::channels_", "channels.register(name)", MappingStatus::kNative},
    {"CommandManager.register", "PluginHost::commands_", "cmd(spec, fn)", MappingStatus::kPartial},
    {"Player.hasPermission", "PluginHost::allowed", "cmd(spec, fn, {permission})", MappingStatus::kPartial},
    {"Scheduler.buildTask", "PluginHost::timers_", "later(), every(), wait()", MappingStatus::kNative},
    {"velocity:player_info", "NativeForwarding (login query)", "none", MappingStatus::kNative},
    {"Plugin jars with velocity-plugin.json, @Subscribe, @DataDirectory", "JarDetector, JavaAdapter over Shim",
     "/plugins lists [Velocity]", MappingStatus::kNative},
    {"EventTask, Continuation", "decision deadline of the plugin thread", "wait() inside on handlers",
     MappingStatus::kNative},
    {"Player.getTabList, setPlayerListHeader, titles, boss bars", "player UI of feat/shared-config-engine", "none",
     MappingStatus::kNeedsNative},
    {"Player.getPermissionValue, CommandSource permissions, brigadier hints",
     "permissions and dispatcher of feat/groups-commands", "none", MappingStatus::kNeedsNative},
    {"Player.storeCookie, requestCookie, transferToHost", "none", "none", MappingStatus::kNeedsNative},
    {"Player.sendResourcePackOffer", "none", "none", MappingStatus::kNeedsNative},
    {"RegisteredServer.ping, ProxyServer.registerServer", "none", "none", MappingStatus::kNeedsNative},

});

} // namespace

std::span<const plugins::AdapterMapping> mappings() noexcept { return kTable; }

} // namespace basalt::proxy::velocity
