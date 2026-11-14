// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import net.md_5.bungee.api.ProxyServer;
import net.md_5.bungee.api.ServerPing;
import net.md_5.bungee.api.chat.BaseComponent;
import net.md_5.bungee.api.chat.TextComponent;
import net.md_5.bungee.api.config.ServerInfo;
import net.md_5.bungee.api.event.ChatEvent;
import net.md_5.bungee.api.event.LoginEvent;
import net.md_5.bungee.api.event.PlayerDisconnectEvent;
import net.md_5.bungee.api.event.PluginMessageEvent;
import net.md_5.bungee.api.event.PostLoginEvent;
import net.md_5.bungee.api.event.PreLoginEvent;
import net.md_5.bungee.api.event.ProxyPingEvent;
import net.md_5.bungee.api.event.ServerConnectEvent;
import net.md_5.bungee.api.event.ServerConnectedEvent;
import net.md_5.bungee.api.event.ServerKickEvent;
import net.md_5.bungee.api.event.ServerSwitchEvent;
import net.md_5.bungee.api.event.SettingsChangedEvent;
import net.md_5.bungee.api.plugin.Plugin;

/** The entry points the native host calls; every result is a flat string array. */
public final class BasaltBungee {
    private static BasaltProxy proxy;
    private static final long WAIT_MILLIS = 2500;

    private static final ClassLoader kPluginContext = new PluginContextLoader();

    private BasaltBungee() {}

    private static void enter() {
        Thread.currentThread().setContextClassLoader(kPluginContext);
    }

    static BasaltProxy proxy() {
        return proxy;
    }

    /** Starts the proxy facade and loads the plugins in the folder. */
    public static void boot(String folder) {
        enter();
        final java.util.logging.Logger root = java.util.logging.Logger.getLogger("");
        for (final java.util.logging.Handler handler : root.getHandlers()) {
            root.removeHandler(handler);
        }
        root.addHandler(new java.util.logging.Handler() {
            @Override
            public void publish(java.util.logging.LogRecord record) {
                final String message = new java.util.logging.SimpleFormatter().formatMessage(record);
                if (message.startsWith("Could not initialize LibraryLoader")) {
                    return;
                }
                Natives.log(record.getThrown() == null ? message : message + ": " + Failures.describe(record.getThrown()));
            }

            @Override
            public void flush() {}

            @Override
            public void close() {}
        });
        proxy = new BasaltProxy(new File(folder));
        ProxyServer.setInstance(proxy);
        proxy.sync();
        proxy.getPluginManager().detectPlugins(bungeeJars(proxy.getPluginsFolder()));
        proxy.getPluginManager().loadPlugins();
        proxy.getPluginManager().enablePlugins();
    }

    /** The facade cannot resolve Maven coordinates itself, so a declared library must already be on the class path. */
    @SuppressWarnings("unchecked")
    private static void warnMissingLibraries(java.util.jar.JarFile file, String origin) {
        final java.util.zip.ZipEntry entry = file.getEntry("bungee.yml") != null ? file.getEntry("bungee.yml") : file.getEntry("plugin.yml");
        try (java.io.InputStream stream = file.getInputStream(entry)) {
            final Object parsed = new org.yaml.snakeyaml.Yaml().load(stream);
            final Object libraries = parsed instanceof java.util.Map<?, ?> map ? map.get("libraries") : null;
            final String path = System.getProperty("java.class.path", "");
            if (libraries instanceof List<?> list) {
                for (final Object item : list) {
                    final String[] parts = String.valueOf(item).split(":");
                    if (parts.length >= 3 && !path.contains(parts[1] + "-" + parts[2] + ".jar")) {
                        Natives.log(origin + " declares the library " + item
                                + ", which is not in .basalt/bungee/lib; run scripts/fetch-plugin-libraries.sh on its jar");
                    }
                }
            }
        } catch (final java.io.IOException | RuntimeException error) {
            Natives.log(origin + " has an unreadable descriptor: " + error);
        }
    }

    /** A folder of links to the jars that are BungeeCord plugins, so other kinds of jar are not tried and reported. */
    private static File bungeeJars(File folder) {
        try {
            final java.nio.file.Path only = java.nio.file.Files.createTempDirectory("basalt-bungee");
            final File[] jars = folder.listFiles((dir, name) -> name.endsWith(".jar"));
            if (jars != null) {
                for (final File jar : jars) {
                    try (java.util.jar.JarFile file = new java.util.jar.JarFile(jar)) {
                        if (file.getEntry("velocity-plugin.json") == null
                                && (file.getEntry("bungee.yml") != null || file.getEntry("plugin.yml") != null)) {
                            warnMissingLibraries(file, jar.getName());
                            java.nio.file.Files.createSymbolicLink(only.resolve(jar.getName()), jar.toPath());
                        }
                    }
                }
            }
            only.toFile().deleteOnExit();
            return only.toFile();
        } catch (final java.io.IOException error) {
            Natives.log("could not list the Bungee plugins: " + error);
            return folder;
        }
    }

    /** Name, version and main class of each loaded plugin. */
    public static String[] plugins() {
        final List<String> rows = new ArrayList<>();
        for (final Plugin plugin : proxy.getPluginManager().getPlugins()) {
            rows.add(plugin.getDescription().getName() + "\t" + plugin.getDescription().getVersion());
        }
        return rows.toArray(new String[0]);
    }

    /** Roots of the commands registered by plugins, console and players alike. */
    public static String[] commands() {
        final List<String> roots = new ArrayList<>();
        for (final java.util.Map.Entry<String, net.md_5.bungee.api.plugin.Command> entry : proxy.getPluginManager().getCommands()) {
            roots.add(entry.getKey());
        }
        return roots.toArray(new String[0]);
    }

    /** Runs a console line; true when it named a registered command. */
    public static boolean console(String line) {
        enter();
        return proxy.getPluginManager().dispatchCommand(proxy.getConsole(), line);
    }

    /** Disables the plugins and stops the scheduler. */
    public static void shutdown() {
        enter();
        for (final Plugin plugin : proxy.getPluginManager().getPlugins()) {
            try {
                plugin.onDisable();
            } catch (final Throwable error) {
                Natives.log(plugin.getDescription().getName() + " failed while disabling: " + error);
            }
        }
        proxy.stopScheduler();
    }

    private static <E extends net.md_5.bungee.api.plugin.Event> E call(E event) {
        return proxy.getPluginManager().callEvent(event);
    }

    private static String[] answer(boolean cancelled, String reasonJson, String server, String text, long max, long online) {
        return new String[] {cancelled ? "1" : "0", reasonJson == null ? "" : reasonJson, server == null ? "" : server,
                text == null ? "" : text, Long.toString(max), Long.toString(online)};
    }

    private static BasaltPlayer playerOf(String[] f) {
        proxy.sync();
        final BasaltPlayer known = proxy.playerNamed(f[0]);
        if (known != null) {
            return known;
        }
        return proxy.remember(new BasaltPlayer(f[0], f[1], f[2], f[3], Integer.parseInt(f[4]), -1));
    }

    private static final String kPingVersion = "Basalt Proxy";

    private static String reasonOf(BaseComponent[] reason) {
        return reason == null || reason.length == 0 ? "" : net.md_5.bungee.chat.ComponentSerializer.toString(reason);
    }

    /** A denial whose reason, or a kick whose new text, is component JSON. */
    private static String[] answerJson(boolean cancelled, String reason, String server, String text) {
        return new String[] {cancelled ? "1" : "0", reason, server == null ? "" : server, text, "-1", "-1", "rt"};
    }

    private static String[] pingAnswer(ServerPing response, int protocol) {
        final BaseComponent description = response.getDescriptionComponent();
        final String motd = description == null ? "" : net.md_5.bungee.chat.ComponentSerializer.toString(description);
        final ServerPing.Players players = response.getPlayers();
        final long online = players == null || players.getOnline() == proxy.getOnlineCount() ? -1 : players.getOnline();
        final ServerPing.Protocol version = response.getVersion();
        final boolean changed = version != null && (!kPingVersion.equals(version.getName()) || version.getProtocol() != protocol);
        final StringBuilder sample = new StringBuilder();
        final boolean sampled = players != null && players.getSample() != null && players.getSample().length > 0;
        if (sampled) {
            for (final ServerPing.PlayerInfo entry : players.getSample()) {
                sample.append(sample.isEmpty() ? "" : "\n").append(entry.getName());
            }
        }
        return new String[] {"0", "", "", motd.equals("{\"text\":\"\"}") ? "" : motd,
                Long.toString(players == null ? -1 : players.getMax()), Long.toString(online), "t",
                changed ? "=" + version.getName() : "", changed ? Integer.toString(version.getProtocol()) : "-1",
                sampled ? "=" + sample : "", response.getFaviconObject() == null ? "" : "=" + response.getFaviconObject().getEncoded()};
    }

    /** kind is login, preconnect, kick, chat, command, ping; f is name, uuid, server, address, port, host, text. */
    static ServerInfo preConnect(BasaltPlayer player, ServerInfo target) {
        final ServerConnectEvent event = new ServerConnectEvent(player, target, ServerConnectEvent.Reason.PLUGIN);
        call(event);
        return event.isCancelled() ? null : event.getTarget();
    }

    public static String[] decide(String kind, String[] f) {
        enter();
        proxy.sync();
        switch (kind) {
            case "login": {
                final BasaltPending pending = new BasaltPending(f[0], f[1], f[3], Integer.parseInt(f[4]), f[5]);
                final CompletableFuture<Boolean> done = new CompletableFuture<>();
                final PreLoginEvent pre = new PreLoginEvent(pending, (result, error) -> done.complete(true));
                call(pre);
                waitFor(done);
                if (pre.isCancelled()) {
                    return answerJson(true, reasonOf(pre.getCancelReasonComponents()), null, "");
                }
                final CompletableFuture<Boolean> second = new CompletableFuture<>();
                final LoginEvent login = new LoginEvent(pending, (result, error) -> second.complete(true));
                call(login);
                waitFor(second);
                return answerJson(login.isCancelled(), login.isCancelled() ? reasonOf(login.getCancelReasonComponents()) : "", null,
                        "");
            }
            case "preconnect": {
                final BasaltPlayer player = playerOf(f);
                final ServerInfo target = proxy.getServerInfo(f[6]);
                if (target == null) {
                    return answer(false, null, null, null, -1, -1);
                }
                final ServerConnectEvent event = new ServerConnectEvent(player, target,
                        player.getServer() == null ? ServerConnectEvent.Reason.JOIN_PROXY : ServerConnectEvent.Reason.PLUGIN);
                call(event);
                return answer(event.isCancelled(), "", event.getTarget() == null ? f[6] : event.getTarget().getName(), null,
                        -1, -1);
            }
            case "kick": {
                final BasaltPlayer player = playerOf(f);
                final ServerInfo from = proxy.getServerInfo(f[2]);
                final ServerKickEvent event = new ServerKickEvent(player, from, new BaseComponent[] {new TextComponent(f[6])},
                        null, ServerKickEvent.State.CONNECTED);
                call(event);
                final ServerInfo redirect = event.isCancelled() ? event.getCancelServer() : null;
                final String text = new TextComponent(event.getKickReasonComponent()).toPlainText();
                return answerJson(false, "", redirect == null ? "" : redirect.getName(),
                        text.equals(f[6]) ? "" : reasonOf(event.getKickReasonComponent()));
            }
            case "chat": {
                final BasaltPlayer player = playerOf(f);
                final ChatEvent event = new ChatEvent(player, player.getServer(), f[6]);
                call(event);
                return answer(event.isCancelled(), "", null, event.getMessage(), -1, -1);
            }
            case "command": {
                final BasaltPlayer player = playerOf(f);
                final boolean handled = proxy.getPluginManager().dispatchCommand(player, f[6]);
                return answer(handled, "", null, null, -1, -1);
            }
            case "ping": {
                final BasaltPending pending = new BasaltPending("", "", f[3], 0, f[5]);
                final int protocol = Integer.parseInt(f[6].isEmpty() ? "0" : f[6]);
                final ServerPing ping = new ServerPing(new ServerPing.Protocol(kPingVersion, protocol),
                        new ServerPing.Players(-1, proxy.getOnlineCount(), null),
                        "", (net.md_5.bungee.api.Favicon) null);
                final CompletableFuture<Boolean> done = new CompletableFuture<>();
                final ProxyPingEvent event = new ProxyPingEvent(pending, ping, (result, error) -> done.complete(true));
                call(event);
                waitFor(done);
                return pingAnswer(event.getResponse(), protocol);
            }
            default:
                return answer(false, null, null, null, -1, -1);
        }
    }

    private static void waitFor(CompletableFuture<Boolean> future) {
        try {
            future.get(WAIT_MILLIS, TimeUnit.MILLISECONDS);
        } catch (final Exception late) {
            Natives.log("an event handler did not finish in time: " + late);
        }
    }

    /** kind is join, switch or quit. */
    public static void notifyPlayer(String kind, String[] f) {
        enter();
        if (kind.equals("settings")) {
            proxy.sync();
            final BasaltPlayer known = proxy.playerNamed(f[0]);
            if (known != null) {
                call(new SettingsChangedEvent(known));
            }
            return;
        }
        final BasaltPlayer player = playerOf(f);
        switch (kind) {
            case "join":
                call(new PostLoginEvent(player, null, (result, error) -> {}));
                if (Natives.settings(player.getName()) != null) {
                    call(new SettingsChangedEvent(player));
                }
                break;
            case "switch":
                call(new ServerSwitchEvent(player, proxy.getServerInfo(f[5])));
                break;
            case "quit":
                call(new PlayerDisconnectEvent(player));
                break;
            default:
                break;
        }
    }

    /** Returns true when a handler cancelled the message. */
    public static boolean message(String player, String channel, byte[] data, boolean fromBackend) {
        enter();
        proxy.sync();
        final BasaltPlayer who = proxy.playerNamed(player);
        if (who == null) {
            return false;
        }
        final PluginMessageEvent event = fromBackend
                ? new PluginMessageEvent(who.getServer(), who, channel, data)
                : new PluginMessageEvent(who, who.getServer(), channel, data);
        call(event);
        return event.isCancelled();
    }

    public static boolean listens(String channel) {
        return proxy.getChannels().contains(channel);
    }
}
