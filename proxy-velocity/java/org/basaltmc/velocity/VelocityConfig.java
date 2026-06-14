// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.velocitypowered.api.proxy.config.ProxyConfig;
import com.velocitypowered.api.util.Favicon;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import net.kyori.adventure.text.Component;

/** The operator settings of proxy.toml and servers.toml as the native proxy read them. */
final class VelocityConfig implements ProxyConfig {
    private final String[] settings = Natives.config();

    @Override
    public boolean isQueryEnabled() {
        return false;
    }

    @Override
    public int getQueryPort() {
        return 0;
    }

    @Override
    public String getQueryMap() {
        return "";
    }

    @Override
    public boolean shouldQueryShowPlugins() {
        return false;
    }

    @Override
    public Component getMotd() {
        return Component.text("Basalt Proxy");
    }

    @Override
    public int getShowMaxPlayers() {
        return Integer.parseInt(settings[6]);
    }

    @Override
    public boolean isOnlineMode() {
        return settings[2].equals("1");
    }

    @Override
    public boolean shouldPreventClientProxyConnections() {
        return settings[7].equals("1");
    }

    @Override
    public Map<String, String> getServers() {
        return Map.of();
    }

    @Override
    public List<String> getAttemptConnectionOrder() {
        return settings[8].isEmpty() ? List.of() : List.of(settings[8].split(","));
    }

    @Override
    public Map<String, List<String>> getForcedHosts() {
        final Map<String, List<String>> hosts = new java.util.LinkedHashMap<>();
        if (!settings[9].isEmpty()) {
            for (final String entry : settings[9].split(";")) {
                final String[] parts = entry.split("=", 2);
                hosts.put(parts[0], parts.length > 1 && !parts[1].isEmpty() ? List.of(parts[1].split(",")) : List.of());
            }
        }
        return hosts;
    }

    @Override
    public int getCompressionThreshold() {
        return Integer.parseInt(settings[3]);
    }

    @Override
    public int getCompressionLevel() {
        return -1;
    }

    @Override
    public int getLoginRatelimit() {
        return 0;
    }

    @Override
    public Optional<Favicon> getFavicon() {
        return Optional.empty();
    }

    @Override
    public boolean isAnnounceForge() {
        return false;
    }

    @Override
    public int getConnectTimeout() {
        return Integer.parseInt(settings[4]);
    }

    @Override
    public int getReadTimeout() {
        return Integer.parseInt(settings[5]);
    }

    @Override
    public int getCommandRatelimit() {
        return 0;
    }

    @Override
    public boolean isForwardCommandsIfRateLimited() {
        return true;
    }

    @Override
    public int getKickAfterRateLimitedCommands() {
        return 0;
    }

    @Override
    public int getKickAfterRateLimitedTabCompletes() {
        return 0;
    }

    @Override
    public int getTabCompleteRatelimit() {
        return 0;
    }
}
