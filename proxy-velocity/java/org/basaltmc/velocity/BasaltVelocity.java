// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.velocitypowered.api.command.CommandSource;
import com.velocitypowered.api.event.ResultedEvent;
import com.velocitypowered.api.event.command.CommandExecuteEvent;
import com.velocitypowered.api.event.connection.DisconnectEvent;
import com.velocitypowered.api.event.connection.LoginEvent;
import com.velocitypowered.api.event.connection.PluginMessageEvent;
import com.velocitypowered.api.event.connection.PostLoginEvent;
import com.velocitypowered.api.event.connection.PreLoginEvent;
import com.velocitypowered.api.event.player.KickedFromServerEvent;
import com.velocitypowered.api.event.player.PlayerChatEvent;
import com.velocitypowered.api.event.player.PlayerChooseInitialServerEvent;
import com.velocitypowered.api.event.player.ServerConnectedEvent;
import com.velocitypowered.api.event.player.ServerPostConnectEvent;
import com.velocitypowered.api.event.player.ServerPreConnectEvent;
import com.velocitypowered.api.event.proxy.ProxyInitializeEvent;
import com.velocitypowered.api.event.proxy.ProxyPingEvent;
import com.velocitypowered.api.event.proxy.ProxyShutdownEvent;
import com.velocitypowered.api.proxy.messages.MinecraftChannelIdentifier;
import com.velocitypowered.api.proxy.server.RegisteredServer;
import com.velocitypowered.api.proxy.server.ServerPing;
import java.io.File;
import java.util.Collection;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import net.kyori.adventure.text.Component;

/** The entry points the native host calls; every result is a flat string array. */
public final class BasaltVelocity {
    private static BasaltProxy proxy;
    private static final Collection<String> CHANNELS = ConcurrentHashMap.newKeySet();

    private BasaltVelocity() {}

    static BasaltProxy proxy() {
        return proxy;
    }

    static Collection<String> channels() {
        return CHANNELS;
    }

    /** Starts the proxy facade, loads the plugins in the folder and tells them the proxy initialized. */
    public static void boot(String folder) {
        proxy = new BasaltProxy(new File(folder));
        proxy.sync();
        proxy.loader().load(proxy.folder());
        proxy.getEventManager().fire(new ProxyInitializeEvent());
    }

    /** Name and version of each loaded plugin, tab separated. */
    public static String[] plugins() {
        return proxy.loader().summary();
    }

    public static String[] commands() {
        return proxy.getCommandManager().getAliases().toArray(new String[0]);
    }

    /** Runs a console line; true when it named a registered command. */
    public static boolean console(String line) {
        return proxy.getCommandManager().executeImmediatelyAsync(proxy.getConsoleCommandSource(), line).join();
    }

    public static void shutdown() {
        proxy.getEventManager().fire(new ProxyShutdownEvent());
        proxy.velocityScheduler().cancelAll();
    }

    private static String[] answer(boolean cancelled, String reason, String server, String text, long max, long online) {
        return new String[] {cancelled ? "1" : "0", reason == null ? "" : reason, server == null ? "" : server,
                text == null ? "" : text, Long.toString(max), Long.toString(online)};
    }

    private static VelocityPlayer playerOf(String[] f) {
        proxy.sync();
        final VelocityPlayer known = proxy.playerNamed(f[0]);
        if (known != null) {
            return known;
        }
        return proxy.remember(new VelocityPlayer(f[0], f[1], f[2], f[3], Integer.parseInt(f[4]), -1));
    }

    private static <E> E fire(E event) {
        return proxy.getEventManager().fire(event).join();
    }

    private static String reasonOf(Optional<Component> reason) {
        return reason.map(Text::mini).orElse("");
    }

    /** kind is login, preconnect, kick, chat, command, ping; f is name, uuid, server, address, port, host, text. */
    static Optional<RegisteredServer> preConnect(VelocityPlayer player, RegisteredServer target) {
        final ServerPreConnectEvent event = fire(new ServerPreConnectEvent(player, target,
                player.getCurrentServer().map(c -> c.getServer()).orElse(null)));
        return event.getResult().isAllowed() ? event.getResult().getServer() : Optional.empty();
    }

    public static String[] decide(String kind, String[] f) {
        proxy.sync();
        switch (kind) {
            case "login": {
                final VelocityPending pending = new VelocityPending(f[3], Integer.parseInt(f[4]), f[5], false);
                final java.util.UUID id = f[1].isEmpty() ? null : java.util.UUID.fromString(VelocityPlayer.dashed(f[1]));
                final PreLoginEvent pre = fire(new PreLoginEvent(pending, f[0], id));
                if (!pre.getResult().isAllowed()) {
                    return answer(true, reasonOf(pre.getResult().getReasonComponent()), null, null, -1, -1);
                }
                final LoginEvent login = fire(new LoginEvent(playerOf(f)));
                return answer(!login.getResult().isAllowed(), reasonOf(login.getResult().getReasonComponent()), null, null, -1, -1);
            }
            case "preconnect": {
                final VelocityPlayer player = playerOf(f);
                final Optional<RegisteredServer> wanted = proxy.getServer(f[6]);
                if (wanted.isEmpty()) {
                    return answer(false, null, null, null, -1, -1);
                }
                RegisteredServer target = wanted.get();
                if (f[2].isEmpty()) {
                    final PlayerChooseInitialServerEvent choose = fire(new PlayerChooseInitialServerEvent(player, target));
                    if (choose.getInitialServer().isEmpty()) {
                        return answer(true, "", null, null, -1, -1);
                    }
                    target = choose.getInitialServer().get();
                }
                final ServerPreConnectEvent event = fire(new ServerPreConnectEvent(player, target, proxy.getServer(f[2]).orElse(null)));
                if (!event.getResult().isAllowed() || event.getResult().getServer().isEmpty()) {
                    return answer(true, "", null, null, -1, -1);
                }
                return answer(false, "", event.getResult().getServer().get().getServerInfo().getName(), null, -1, -1);
            }
            case "kick": {
                final VelocityPlayer player = playerOf(f);
                final Optional<RegisteredServer> from = proxy.getServer(f[2]);
                if (from.isEmpty()) {
                    return answer(false, null, null, null, -1, -1);
                }
                final Component original = Text.ofMini(f[6]);
                final KickedFromServerEvent event = fire(new KickedFromServerEvent(player, from.get(), original, false,
                        KickedFromServerEvent.DisconnectPlayer.create(original)));
                final KickedFromServerEvent.ServerKickResult result = event.getResult();
                if (result instanceof KickedFromServerEvent.RedirectPlayer redirect) {
                    return answer(false, null, redirect.getServer().getServerInfo().getName(), null, -1, -1);
                }
                if (result instanceof KickedFromServerEvent.DisconnectPlayer disconnect) {
                    final String text = Text.mini(disconnect.getReasonComponent());
                    return answer(false, text.equals(Text.mini(original)) ? "" : text, null, null, -1, -1);
                }
                return answer(false, null, null, null, -1, -1);
            }
            case "chat": {
                final PlayerChatEvent event = fire(new PlayerChatEvent(playerOf(f), f[6]));
                return answer(!event.getResult().isAllowed(), "", null, event.getResult().getMessage().orElse(null), -1, -1);
            }
            case "command": {
                final VelocityPlayer player = playerOf(f);
                final CommandExecuteEvent event = fire(new CommandExecuteEvent(player, f[6]));
                final CommandExecuteEvent.CommandResult result = event.getResult();
                if (!result.isAllowed()) {
                    return answer(true, "", null, null, -1, -1);
                }
                final String line = result.getCommand().orElse(f[6]);
                if (result.isForwardToServer()) {
                    return answer(false, "", null, result.getCommand().orElse(null), -1, -1);
                }
                final boolean handled = proxy.getCommandManager().executeImmediatelyAsync((CommandSource) player, line).join();
                return answer(handled, "", null, handled ? null : result.getCommand().orElse(null), -1, -1);
            }
            case "ping": {
                final VelocityPending pending = new VelocityPending(f[3], 0, f[5], true);
                final ServerPing ping = ServerPing.builder()
                        .version(new ServerPing.Version(f[6].isEmpty() ? 0 : Integer.parseInt(f[6]), "Basalt Proxy"))
                        .onlinePlayers(proxy.getPlayerCount()).maximumPlayers(-1).description(Component.empty()).build();
                final ProxyPingEvent event = fire(new ProxyPingEvent(pending, ping));
                final ServerPing response = event.getPing();
                final String motd = Text.mini(response.getDescriptionComponent());
                final long max = response.getPlayers().map(ServerPing.Players::getMax).orElse(-1);
                final long online = response.getPlayers().map(ServerPing.Players::getOnline).filter(n -> n != proxy.getPlayerCount()).orElse(-1);
                return answer(false, null, null, motd, max, online);
            }
            default:
                return answer(false, null, null, null, -1, -1);
        }
    }

    /** kind is join, switch or quit. */
    public static void notifyPlayer(String kind, String[] f) {
        final VelocityPlayer player = playerOf(f);
        switch (kind) {
            case "join":
                fire(new PostLoginEvent(player));
                proxy.getServer(f[6]).ifPresent(server -> {
                    fire(new ServerConnectedEvent(player, server, null));
                    fire(new ServerPostConnectEvent(player, null));
                });
                break;
            case "switch":
                proxy.getServer(f[6]).ifPresent(server -> {
                    fire(new ServerConnectedEvent(player, server, null));
                    fire(new ServerPostConnectEvent(player, null));
                });
                break;
            case "quit":
                fire(new DisconnectEvent(player, DisconnectEvent.LoginStatus.SUCCESSFUL_LOGIN));
                break;
            default:
                break;
        }
    }

    /** Returns true when a handler took the message over. */
    public static boolean message(String name, String channel, byte[] data, boolean fromBackend) {
        proxy.sync();
        final VelocityPlayer player = proxy.playerNamed(name);
        if (player == null || !CHANNELS.contains(channel)) {
            return false;
        }
        final com.velocitypowered.api.proxy.messages.ChannelIdentifier identifier = channel.indexOf(':') >= 0
                ? MinecraftChannelIdentifier.from(channel)
                : new com.velocitypowered.api.proxy.messages.LegacyChannelIdentifier(channel);
        final PluginMessageEvent event = fromBackend
                ? new PluginMessageEvent(player.getCurrentServer().orElse(null), player, identifier, data)
                : new PluginMessageEvent(player, player.getCurrentServer().orElse(null), identifier, data);
        fire(event);
        return !event.getResult().isAllowed();
    }
}
