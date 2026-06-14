// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.velocitypowered.api.command.CommandManager;
import com.velocitypowered.api.event.EventManager;
import com.velocitypowered.api.plugin.PluginManager;
import com.velocitypowered.api.proxy.ConsoleCommandSource;
import com.velocitypowered.api.proxy.Player;
import com.velocitypowered.api.proxy.ProxyServer;
import com.velocitypowered.api.proxy.config.ProxyConfig;
import com.velocitypowered.api.proxy.messages.ChannelRegistrar;
import com.velocitypowered.api.proxy.player.ResourcePackInfo;
import com.velocitypowered.api.proxy.server.RegisteredServer;
import com.velocitypowered.api.proxy.server.ServerInfo;
import com.velocitypowered.api.scheduler.Scheduler;
import com.velocitypowered.api.util.ProxyVersion;
import java.io.File;
import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import net.kyori.adventure.text.Component;

final class BasaltProxy implements ProxyServer {
    private final Map<String, VelocityPlayer> players = new ConcurrentHashMap<>();
    private final Map<String, RegisteredServer> servers = new LinkedHashMap<>();
    private final VelocityConsole console = new VelocityConsole();
    private final VelocityEvents events = new VelocityEvents();
    private final VelocityCommands commands = new VelocityCommands();
    private final VelocityScheduler scheduler = new VelocityScheduler();
    private final VelocityChannels channels = new VelocityChannels();
    private final VelocityConfig config = new VelocityConfig();
    private final VelocityPlugins plugins = new VelocityPlugins(this);
    private final File folder;

    BasaltProxy(File folder) {
        this.folder = folder;
    }

    File folder() {
        return folder;
    }

    VelocityPlugins loader() {
        return plugins;
    }

    VelocityScheduler velocityScheduler() {
        return scheduler;
    }

    VelocityPlayer playerNamed(String name) {
        return players.get(name.toLowerCase(Locale.ROOT));
    }

    VelocityPlayer remember(VelocityPlayer player) {
        final VelocityPlayer known = players.putIfAbsent(player.getUsername().toLowerCase(Locale.ROOT), player);
        return known == null ? player : known;
    }

    void sync() {
        final Map<String, String[]> fresh = new LinkedHashMap<>();
        for (final String[] row : Natives.players()) {
            fresh.put(row[0].toLowerCase(Locale.ROOT), row);
        }
        players.keySet().removeIf(key -> !fresh.containsKey(key));
        for (final Map.Entry<String, String[]> entry : fresh.entrySet()) {
            final String[] row = entry.getValue();
            final VelocityPlayer known = players.get(entry.getKey());
            if (known == null) {
                players.put(entry.getKey(), new VelocityPlayer(row[0], row[1], row[2], row[3], Integer.parseInt(row[4]), Long.parseLong(row[5])));
            } else {
                known.update(row[2], Long.parseLong(row[5]));
            }
        }
        servers.clear();
        for (final String name : Natives.servers()) {
            servers.put(name, new VelocityServer(name));
        }
    }

    @Override
    public void shutdown(Component reason) {
        throw new UnsupportedOperationException("stop the proxy from its console");
    }

    @Override
    public void shutdown() {
        shutdown(Component.empty());
    }

    @Override
    public boolean isShuttingDown() {
        return false;
    }

    @Override
    public void closeListeners() {}

    @Override
    public Optional<Player> getPlayer(String name) {
        return Optional.ofNullable(playerNamed(name));
    }

    @Override
    public Optional<Player> getPlayer(UUID id) {
        for (final VelocityPlayer player : players.values()) {
            if (player.getUniqueId().equals(id)) {
                return Optional.of(player);
            }
        }
        return Optional.empty();
    }

    @Override
    public Collection<Player> getAllPlayers() {
        return new ArrayList<>(players.values());
    }

    @Override
    public int getPlayerCount() {
        return players.size();
    }

    @Override
    public Optional<RegisteredServer> getServer(String name) {
        return Optional.ofNullable(servers.get(name));
    }

    @Override
    public Collection<RegisteredServer> getAllServers() {
        return new ArrayList<>(servers.values());
    }

    @Override
    public Collection<Player> matchPlayer(String partial) {
        final Collection<Player> found = new ArrayList<>();
        for (final VelocityPlayer player : players.values()) {
            if (player.getUsername().toLowerCase(Locale.ROOT).contains(partial.toLowerCase(Locale.ROOT))) {
                found.add(player);
            }
        }
        return found;
    }

    @Override
    public Collection<RegisteredServer> matchServer(String partial) {
        final Collection<RegisteredServer> found = new ArrayList<>();
        for (final RegisteredServer server : servers.values()) {
            if (server.getServerInfo().getName().toLowerCase(Locale.ROOT).contains(partial.toLowerCase(Locale.ROOT))) {
                found.add(server);
            }
        }
        return found;
    }

    @Override
    public RegisteredServer createRawRegisteredServer(ServerInfo info) {
        return new VelocityServer(info.getName());
    }

    @Override
    public RegisteredServer registerServer(ServerInfo info) {
        throw new UnsupportedOperationException("servers are registered in servers.toml");
    }

    @Override
    public void unregisterServer(ServerInfo info) {
        throw new UnsupportedOperationException("servers are registered in servers.toml");
    }

    @Override
    public ConsoleCommandSource getConsoleCommandSource() {
        return console;
    }

    @Override
    public PluginManager getPluginManager() {
        return plugins;
    }

    @Override
    public EventManager getEventManager() {
        return events;
    }

    @Override
    public CommandManager getCommandManager() {
        return commands;
    }

    @Override
    public Scheduler getScheduler() {
        return scheduler;
    }

    @Override
    public ChannelRegistrar getChannelRegistrar() {
        return channels;
    }

    @Override
    public InetSocketAddress getBoundAddress() {
        final String[] settings = Natives.config();
        return new InetSocketAddress(settings[0], Integer.parseInt(settings[1]));
    }

    @Override
    public ProxyConfig getConfiguration() {
        return config;
    }

    @Override
    public ProxyVersion getVersion() {
        return new ProxyVersion("Basalt Proxy", "Basalt", "26.3");
    }

    @Override
    public ResourcePackInfo.Builder createResourcePackBuilder(String url) {
        throw new UnsupportedOperationException("resource packs are not available yet");
    }

    @Override
    public void sendMessage(Component message) {
        for (final VelocityPlayer player : players.values()) {
            player.sendMessage(message);
        }
    }
}
