#include <basalt/proxy/bungee/BungeeMapping.hpp>

#include <array>

namespace basalt::proxy::bungee {
namespace {

using plugins::MappingStatus;

constexpr std::array kTable = std::to_array<plugins::AdapterMapping>({
    {"PostLoginEvent", "PlayerEventKind::kJoin", "on('player.join')", MappingStatus::kNative},
    {"PlayerDisconnectEvent", "PlayerEventKind::kLeave", "on('player.quit')", MappingStatus::kNative},
    {"ServerSwitchEvent", "PlayerEventKind::kSwitch", "on('player.server_change')", MappingStatus::kNative},
    {"ServerKickEvent", "DecisionKind::kKick", "on('player.kick') with e.server, e.reason", MappingStatus::kNative},
    {"ChatEvent", "DecisionKind::kChat/kCommand", "on('player.chat'), on('player.command') with e.cancelled",
     MappingStatus::kNative},
    {"ProxyPingEvent", "DecisionKind::kPing", "on('server.ping') with e.motd, e.online, e.max_players",
     MappingStatus::kNative},
    {"LoginEvent", "DecisionKind::kLogin", "on('player.login')", MappingStatus::kNative},
    {"PreLoginEvent", "DecisionKind::kLogin", "on('player.login') with e.cancelled, e.reason", MappingStatus::kNative},
    {"ServerConnectEvent", "DecisionKind::kPreConnect", "on('player.server_pre_connect') with e.server, e.cancelled",
     MappingStatus::kNative},
    {"ServerConnectedEvent", "PlayerEventKind::kSwitch", "on('player.server_change')", MappingStatus::kNative},
    {"PluginMessageEvent", "PluginHost::message", "on('player.message')", MappingStatus::kPartial},
    {"ProxiedPlayer.getName/getUniqueId", "PluginHost::Player", "player.name, player.uuid", MappingStatus::kNative},
    {"ProxiedPlayer.getAddress", "PluginHost::Player", "player.address, player.port", MappingStatus::kNative},
    {"ProxiedPlayer.getServer", "PluginHost::Player", "player.server", MappingStatus::kNative},
    {"ProxiedPlayer.connect", "hooks.send", "player:connect(server)", MappingStatus::kNative},
    {"ProxiedPlayer.disconnect", "hooks.kick", "player:kick(reason)", MappingStatus::kNative},
    {"ProxiedPlayer.sendMessage", "hooks.tell", "player:send(text)", MappingStatus::kNative},
    {"ProxiedPlayer.sendData", "hooks.sendMessage", "player:message(channel, data)", MappingStatus::kNative},
    {"ProxiedPlayer.getPing", "ClientSession keepalive RTT", "player.ping", MappingStatus::kNative},
    {"ProxyServer.getPlayers/getOnlineCount", "PluginHost::roster_", "players.online(), players.count()",
     MappingStatus::kNative},
    {"ProxyServer.getServers", "hooks.servers", "servers.list()", MappingStatus::kNative},
    {"ServerInfo.getSocketAddress", "hooks.serverAddress", "servers.address(name)", MappingStatus::kNative},
    {"ServerInfo.getPlayers", "PluginHost::playersOn", "servers.players(name)", MappingStatus::kNative},
    {"ProxyServer.registerChannel", "PluginHost::channels_", "channels.register(name)", MappingStatus::kNative},
    {"ProxyServer.getPluginManager().registerCommand", "PluginHost::commands_", "cmd(spec, fn)",
     MappingStatus::kPartial},
    {"CommandSender.hasPermission", "PluginHost::allowed", "cmd(spec, fn, {permission})", MappingStatus::kPartial},
    {"ProxyServer.getScheduler", "PluginHost::timers_", "later(), every(), wait()", MappingStatus::kNative},
    {"BungeeCord channel: Connect, ConnectOther", "handleBungeeMessage", "player:connect(server)",
     MappingStatus::kNative},
    {"BungeeCord channel: IP, IPOther, UUID, UUIDOther", "handleBungeeMessage", "player.address, player.uuid",
     MappingStatus::kNative},
    {"BungeeCord channel: PlayerCount, PlayerList", "handleBungeeMessage", "players.count(server), servers.players()",
     MappingStatus::kNative},
    {"BungeeCord channel: GetServers, GetServer, ServerIP", "handleBungeeMessage", "servers.list(), servers.address()",
     MappingStatus::kNative},
    {"BungeeCord channel: Message", "handleBungeeMessage", "player:send(text)", MappingStatus::kNative},
    {"BungeeCord channel: Forward, ForwardToPlayer", "handleBungeeMessage", "player:message(channel, data)",
     MappingStatus::kNative},
    {"BungeeCord channel: KickPlayer", "handleBungeeMessage", "player:kick(reason)", MappingStatus::kNative},
    {"BungeeCord channel: MessageRaw, KickPlayerRaw", "tellComponent, kickComponent",
     "player:send_json(json), player:kick_json(json)", MappingStatus::kNative},
    {"Plugin jars with bungee.yml or plugin.yml", "JarDetector, BungeeAdapter over Shim", "/plugins lists [Bungee]",
     MappingStatus::kNative},
    {"ProxiedPlayer.sendTitle, setTabHeader, getScoreboard", "player UI of feat/shared-config-engine", "none",
     MappingStatus::kNeedsNative},
    {"ProxiedPlayer.retrieveCookie, storeCookie, transfer", "none", "none", MappingStatus::kNeedsNative},
    {"ProxiedPlayer.showDialog, sendServerLinks", "none", "none", MappingStatus::kNeedsNative},
    {"ProxiedPlayer.chat, Connection.Unsafe.sendPacket", "none", "none", MappingStatus::kNeedsNative},
    {"ProxiedPlayer.hasPermission, getGroups, setPermission", "permissions of feat/groups-commands", "none",
     MappingStatus::kNeedsNative},
});

} // namespace

std::span<const plugins::AdapterMapping> mappings() noexcept { return kTable; }

} // namespace basalt::proxy::bungee
