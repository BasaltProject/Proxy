// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import java.io.File;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.logging.Logger;
import net.md_5.bungee.api.CommandSender;
import net.md_5.bungee.api.ProxyConfig;
import net.md_5.bungee.api.ProxyServer;
import net.md_5.bungee.api.ReconnectHandler;
import net.md_5.bungee.api.Title;
import net.md_5.bungee.api.chat.BaseComponent;
import net.md_5.bungee.api.chat.TextComponent;
import net.md_5.bungee.api.config.ConfigurationAdapter;
import net.md_5.bungee.api.config.ServerInfo;
import net.md_5.bungee.api.connection.ProxiedPlayer;
import net.md_5.bungee.api.plugin.PluginManager;
import net.md_5.bungee.api.scheduler.TaskScheduler;

final class BasaltProxy extends ProxyServer {
    private final Logger logger = Logger.getLogger("BungeeCord");
    private final BasaltConfig config = new BasaltConfig();
    private final BasaltConsole console = new BasaltConsole();
    private final BasaltScheduler scheduler = new BasaltScheduler();
    private final File pluginsFolder;
    private final Map<String, BasaltPlayer> players = new ConcurrentHashMap<>();
    private final Map<String, ServerInfo> servers = new LinkedHashMap<>();
    private final Collection<String> channels = ConcurrentHashMap.newKeySet();
    private final PluginManager pluginManager;
    private ConfigurationAdapter adapter = config;
    private ReconnectHandler reconnect;

    BasaltProxy(File pluginsFolder) {
        this.pluginsFolder = pluginsFolder;
        this.pluginManager = new PluginManager(this);
    }

    void stopScheduler() {
        scheduler.stop();
    }

    BasaltPlayer playerNamed(String name) {
        return players.get(name.toLowerCase(Locale.ROOT));
    }

    void sync() {
        final Map<String, String[]> fresh = new LinkedHashMap<>();
        for (final String[] row : Natives.players()) {
            fresh.put(row[0].toLowerCase(Locale.ROOT), row);
        }
        players.entrySet().removeIf(entry -> {
            if (!fresh.containsKey(entry.getKey())) {
                entry.getValue().gone();
                return true;
            }
            return false;
        });
        for (final Map.Entry<String, String[]> entry : fresh.entrySet()) {
            final String[] row = entry.getValue();
            final BasaltPlayer known = players.get(entry.getKey());
            if (known == null) {
                players.put(entry.getKey(), new BasaltPlayer(row[0], row[1], row[2], row[3], Integer.parseInt(row[4]),
                        Integer.parseInt(row[5])));
            } else {
                known.update(row[2], Integer.parseInt(row[5]));
            }
        }
        servers.clear();
        for (final String name : Natives.servers()) {
            servers.put(name, new BasaltServerInfo(name));
        }
    }

    BasaltPlayer remember(BasaltPlayer player) {
        players.putIfAbsent(player.getName().toLowerCase(Locale.ROOT), player);
        return players.get(player.getName().toLowerCase(Locale.ROOT));
    }

    @Override
    public String getName() {
        return "Basalt Proxy";
    }

    @Override
    public String getVersion() {
        return "26.3";
    }

    @Override
    public String getTranslation(String name, Object... args) {
        return name;
    }

    @Override
    public Logger getLogger() {
        return logger;
    }

    @Override
    public Collection<ProxiedPlayer> getPlayers() {
        return new ArrayList<>(players.values());
    }

    @Override
    public ProxiedPlayer getPlayer(String name) {
        return playerNamed(name);
    }

    @Override
    public ProxiedPlayer getPlayer(UUID id) {
        for (final BasaltPlayer player : players.values()) {
            if (player.getUniqueId().equals(id)) {
                return player;
            }
        }
        return null;
    }

    @Override
    public Map<String, ServerInfo> getServers() {
        return servers;
    }

    @Override
    public ServerInfo getServerInfo(String name) {
        return servers.get(name);
    }

    @Override
    public PluginManager getPluginManager() {
        return pluginManager;
    }

    @Override
    public ConfigurationAdapter getConfigurationAdapter() {
        return adapter;
    }

    @Override
    public void setConfigurationAdapter(ConfigurationAdapter value) {
        adapter = value;
    }

    @Override
    public ReconnectHandler getReconnectHandler() {
        return reconnect;
    }

    @Override
    public void setReconnectHandler(ReconnectHandler handler) {
        reconnect = handler;
    }

    @Override
    public void stop() {
        stop("Proxy stopped");
    }

    @Override
    public void stop(String reason) {
        throw new UnsupportedOperationException("stop the proxy from its console");
    }

    @Override
    public void registerChannel(String channel) {
        channels.add(channel);
        Natives.channel(channel, true);
    }

    @Override
    public void unregisterChannel(String channel) {
        channels.remove(channel);
        Natives.channel(channel, false);
    }

    @Override
    public Collection<String> getChannels() {
        return channels;
    }

    @Override
    public String getGameVersion() {
        return "26.3";
    }

    @Override
    public int getProtocolVersion() {
        return 777;
    }

    @Override
    public ServerInfo constructServerInfo(String name, InetSocketAddress address, String motd, boolean restricted) {
        return new BasaltServerInfo(name);
    }

    @Override
    public ServerInfo constructServerInfo(String name, SocketAddress address, String motd, boolean restricted) {
        return new BasaltServerInfo(name);
    }

    @Override
    public CommandSender getConsole() {
        return console;
    }

    @Override
    public File getPluginsFolder() {
        return pluginsFolder;
    }

    @Override
    public TaskScheduler getScheduler() {
        return scheduler;
    }

    @Override
    public int getOnlineCount() {
        return players.size();
    }

    @Override
    public void broadcast(String message) {
        broadcast(TextComponent.fromLegacy(message));
    }

    @Override
    public void broadcast(BaseComponent... message) {
        for (final ProxiedPlayer player : players.values()) {
            player.sendMessage(message);
        }
    }

    @Override
    public void broadcast(BaseComponent message) {
        for (final ProxiedPlayer player : players.values()) {
            player.sendMessage(message);
        }
    }

    @Override
    public Collection<String> getDisabledCommands() {
        return List.of();
    }

    @Override
    public ProxyConfig getConfig() {
        return config;
    }

    @Override
    public Collection<ProxiedPlayer> matchPlayer(String partial) {
        final List<ProxiedPlayer> found = new ArrayList<>();
        for (final BasaltPlayer player : players.values()) {
            if (player.getName().toLowerCase(Locale.ROOT).contains(partial.toLowerCase(Locale.ROOT))) {
                found.add(player);
            }
        }
        return found;
    }

    @Override
    public Title createTitle() {
        return new BasaltTitle();
    }

    @Override
    public ProxyServer.Unsafe unsafe() {
        throw new UnsupportedOperationException("raw packets are not available");
    }
}
