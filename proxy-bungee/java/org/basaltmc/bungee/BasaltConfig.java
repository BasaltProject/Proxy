// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import java.net.InetSocketAddress;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import net.md_5.bungee.api.ProxyConfig;
import net.md_5.bungee.api.config.ConfigurationAdapter;
import net.md_5.bungee.api.config.ListenerInfo;
import net.md_5.bungee.api.config.ServerInfo;

final class BasaltConfig implements ProxyConfig, ConfigurationAdapter {
    private final String[] settings = Natives.config();
    private final List<String> route = settings[8].isEmpty() ? List.of() : List.of(settings[8].split(","));
    private final ListenerInfo listener = new ListenerInfo(new InetSocketAddress(settings[0], Integer.parseInt(settings[1])),
            "Basalt Proxy", Integer.parseInt(settings[6]), 60, route, false, forcedHosts(settings[9]), "DEFAULT", false,
            false, 0, false);

    private static Map<String, String> forcedHosts(String text) {
        final Map<String, String> hosts = new java.util.LinkedHashMap<>();
        if (!text.isEmpty()) {
            for (final String entry : text.split(";")) {
                final String[] parts = entry.split("=", 2);
                hosts.put(parts[0], parts.length > 1 ? parts[1].split(",")[0] : "");
            }
        }
        return hosts;
    }

    @Override
    public int getTimeout() {
        return Integer.parseInt(settings[5]);
    }

    @Override
    public String getUuid() {
        return "00000000-0000-0000-0000-000000000000";
    }

    @Override
    public Collection<ListenerInfo> getListeners() {
        return List.of(listener);
    }

    @Override
    public Map<String, ServerInfo> getServers() {
        return BasaltBungee.proxy().getServers();
    }

    @Override
    public boolean isOnlineMode() {
        return settings[2].equals("1");
    }

    @Override
    public boolean isLogCommands() {
        return false;
    }

    @Override
    public int getRemotePingCache() {
        return 0;
    }

    @Override
    public int getPlayerLimit() {
        return Integer.parseInt(settings[6]);
    }

    @Override
    public Collection<String> getDisabledCommands() {
        return List.of();
    }

    @Override
    public int getServerConnectTimeout() {
        return Integer.parseInt(settings[4]);
    }

    @Override
    public int getRemotePingTimeout() {
        return 5000;
    }

    @Override
    public int getThrottle() {
        return 0;
    }

    @Override
    public boolean isIpForward() {
        return false;
    }

    @Override
    public String getFavicon() {
        return null;
    }

    @Override
    public net.md_5.bungee.api.Favicon getFaviconObject() {
        return null;
    }

    @Override
    public void load() {}

    @Override
    public int getInt(String path, int def) {
        return def;
    }

    @Override
    public String getString(String path, String def) {
        return def;
    }

    @Override
    public boolean getBoolean(String path, boolean def) {
        return def;
    }

    @Override
    public Collection<?> getList(String path, Collection<?> def) {
        return def;
    }

    @Override
    public Collection<String> getGroups(String player) {
        return List.of();
    }

    @Override
    public Collection<String> getPermissions(String group) {
        return List.of();
    }
}
