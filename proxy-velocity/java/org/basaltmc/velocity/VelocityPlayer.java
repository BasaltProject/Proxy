// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.google.common.io.ByteArrayDataOutput;
import com.google.common.io.ByteStreams;
import com.velocitypowered.api.network.HandshakeIntent;
import com.velocitypowered.api.network.ProtocolState;
import com.velocitypowered.api.network.ProtocolVersion;
import com.velocitypowered.api.permission.Tristate;
import com.velocitypowered.api.proxy.ConnectionRequestBuilder;
import com.velocitypowered.api.proxy.Player;
import com.velocitypowered.api.proxy.ServerConnection;
import com.velocitypowered.api.proxy.messages.ChannelIdentifier;
import com.velocitypowered.api.proxy.messages.PluginMessageEncoder;
import com.velocitypowered.api.proxy.player.PlayerSettings;
import com.velocitypowered.api.proxy.player.ResourcePackInfo;
import com.velocitypowered.api.proxy.player.TabList;
import com.velocitypowered.api.proxy.server.RegisteredServer;
import com.velocitypowered.api.util.GameProfile;
import com.velocitypowered.api.util.ModInfo;
import com.velocitypowered.api.util.ServerLink;
import java.net.InetSocketAddress;
import java.util.Collection;
import java.util.List;
import java.util.Locale;
import java.util.Optional;
import java.util.UUID;
import net.kyori.adventure.identity.Identity;
import net.kyori.adventure.text.Component;

final class VelocityPlayer implements Player {
    private final String name;
    private final UUID uuid;
    private final String address;
    private final int port;
    private volatile String server;
    private volatile long ping;
    private final UUID session = UUID.randomUUID();
    private Locale locale = Locale.ENGLISH;

    VelocityPlayer(String name, String uuidText, String server, String address, int port, long ping) {
        this.name = name;
        this.uuid = uuidText == null || uuidText.isEmpty() ? new UUID(0, 0) : UUID.fromString(dashed(uuidText));
        this.server = server;
        this.address = address;
        this.port = port;
        this.ping = ping;
    }

    static String dashed(String text) {
        if (text.indexOf('-') >= 0 || text.length() != 32) {
            return text;
        }
        return text.substring(0, 8) + "-" + text.substring(8, 12) + "-" + text.substring(12, 16) + "-" + text.substring(16, 20)
                + "-" + text.substring(20);
    }

    void update(String server, long ping) {
        this.server = server;
        this.ping = ping;
    }

    private static UnsupportedOperationException missing(String what) {
        return new UnsupportedOperationException(what + " is not available yet");
    }

    @Override
    public String getUsername() {
        return name;
    }

    @Override
    public Locale getEffectiveLocale() {
        return locale;
    }

    @Override
    public void setEffectiveLocale(Locale value) {
        locale = value;
    }

    @Override
    public UUID getUniqueId() {
        return uuid;
    }

    @Override
    public Optional<ServerConnection> getCurrentServer() {
        if (server == null || server.isEmpty()) {
            return Optional.empty();
        }
        return Optional.of(new VelocityConnection(this, new VelocityServer(server)));
    }

    @Override
    public PlayerSettings getPlayerSettings() {
        throw missing("player settings");
    }

    @Override
    public boolean hasSentPlayerSettings() {
        return false;
    }

    @Override
    public Optional<ModInfo> getModInfo() {
        return Optional.empty();
    }

    @Override
    public long getPing() {
        return ping;
    }

    @Override
    public boolean isOnlineMode() {
        return false;
    }

    @Override
    public ConnectionRequestBuilder createConnectionRequest(RegisteredServer target) {
        return new ConnectionRequestBuilder() {
            @Override
            public RegisteredServer getServer() {
                return target;
            }

            @Override
            public java.util.concurrent.CompletableFuture<Result> connect() {
                final boolean sent = go();
                return java.util.concurrent.CompletableFuture.completedFuture(new Result() {
                    @Override
                    public Status getStatus() {
                        return sent ? Status.SUCCESS : Status.CONNECTION_CANCELLED;
                    }

                    @Override
                    public Optional<Component> getReasonComponent() {
                        return Optional.empty();
                    }

                    @Override
                    public RegisteredServer getAttemptedConnection() {
                        return target;
                    }
                });
            }

            @Override
            public java.util.concurrent.CompletableFuture<Boolean> connectWithIndication() {
                return connect().thenApply(Result::isSuccessful);
            }

            @Override
            public void fireAndForget() {
                go();
            }

            private boolean go() {
                final Optional<RegisteredServer> chosen = BasaltVelocity.preConnect(VelocityPlayer.this, target);
                return chosen.isPresent() && Natives.connect(name, chosen.get().getServerInfo().getName());
            }
        };
    }

    @Override
    public List<GameProfile.Property> getGameProfileProperties() {
        return List.of();
    }

    @Override
    public void setGameProfileProperties(List<GameProfile.Property> properties) {
        throw missing("profile properties");
    }

    @Override
    public GameProfile getGameProfile() {
        return new GameProfile(uuid, name, List.of());
    }

    @Override
    public void clearPlayerListHeaderAndFooter() {
        getTabList().clearHeaderAndFooter();
    }

    @Override
    public void sendPlayerListHeaderAndFooter(Component header, Component footer) {
        getTabList().setHeaderAndFooter(header, footer);
    }

    @Override
    public void showTitle(net.kyori.adventure.title.Title title) {
        final net.kyori.adventure.title.Title.Times times = title.times();
        Hud.send(name, Hud.TITLE, Hud.json(title.title()), Hud.json(title.subtitle()),
                times == null ? -1 : (int) (times.fadeIn().toMillis() / 50), times == null ? -1 : (int) (times.stay().toMillis() / 50),
                times == null ? -1 : (int) (times.fadeOut().toMillis() / 50), 0, null);
    }

    @Override
    public void clearTitle() {
        Hud.send(name, Hud.CLEAR_TITLE, null, null, 0, 0, 0, 0, null);
    }

    @Override
    public void resetTitle() {
        Hud.send(name, Hud.CLEAR_TITLE, null, null, 1, 0, 0, 0, null);
    }

    @Override
    public void sendActionBar(Component message) {
        Hud.send(name, Hud.ACTION_BAR, Hud.json(message), null, 0, 0, 0, 0, null);
    }

    private final java.util.Map<net.kyori.adventure.bossbar.BossBar, String> bars = new java.util.concurrent.ConcurrentHashMap<>();

    @Override
    public void showBossBar(net.kyori.adventure.bossbar.BossBar bar) {
        final String id = bars.computeIfAbsent(bar, key -> UUID.randomUUID().toString());
        Hud.send(name, Hud.BOSS_ADD, id, Hud.json(bar.name()), Hud.color(bar.color()), Hud.overlay(bar.overlay()), 0, bar.progress(), null);
        bar.addListener(new net.kyori.adventure.bossbar.BossBar.Listener() {
            @Override
            public void bossBarNameChanged(net.kyori.adventure.bossbar.BossBar changed, Component was, Component now) {
                if (bars.containsKey(changed)) {
                    Hud.send(name, Hud.BOSS_NAME, id, Hud.json(now), 0, 0, 0, 0, null);
                }
            }

            @Override
            public void bossBarProgressChanged(net.kyori.adventure.bossbar.BossBar changed, float was, float now) {
                if (bars.containsKey(changed)) {
                    Hud.send(name, Hud.BOSS_PROGRESS, id, null, 0, 0, 0, now, null);
                }
            }

            @Override
            public void bossBarColorChanged(net.kyori.adventure.bossbar.BossBar changed, net.kyori.adventure.bossbar.BossBar.Color was,
                    net.kyori.adventure.bossbar.BossBar.Color now) {
                style(changed);
            }

            @Override
            public void bossBarOverlayChanged(net.kyori.adventure.bossbar.BossBar changed, net.kyori.adventure.bossbar.BossBar.Overlay was,
                    net.kyori.adventure.bossbar.BossBar.Overlay now) {
                style(changed);
            }

            private void style(net.kyori.adventure.bossbar.BossBar changed) {
                if (bars.containsKey(changed)) {
                    Hud.send(name, Hud.BOSS_STYLE, id, null, Hud.color(changed.color()), Hud.overlay(changed.overlay()), 0, 0, null);
                }
            }
        });
    }

    @Override
    public void hideBossBar(net.kyori.adventure.bossbar.BossBar bar) {
        final String id = bars.remove(bar);
        if (id != null) {
            Hud.send(name, Hud.BOSS_REMOVE, id, null, 0, 0, 0, 0, null);
        }
    }

    @Override
    public Component getPlayerListHeader() {
        return Component.empty();
    }

    @Override
    public Component getPlayerListFooter() {
        return Component.empty();
    }

    @Override
    public TabList getTabList() {
        return new VelocityTabList(name);
    }

    @Override
    public void disconnect(Component reason) {
        Natives.kick(name, Text.json(reason));
    }

    @Override
    public void spoofChatInput(String input) {
        throw missing("spoofing chat");
    }

    @Override
    public void sendResourcePack(String url) {
        throw missing("resource packs");
    }

    @Override
    public void sendResourcePack(String url, byte[] hash) {
        throw missing("resource packs");
    }

    @Override
    public void sendResourcePackOffer(ResourcePackInfo info) {
        throw missing("resource packs");
    }

    @Override
    public ResourcePackInfo getAppliedResourcePack() {
        return null;
    }

    @Override
    public ResourcePackInfo getPendingResourcePack() {
        return null;
    }

    @Override
    public Collection<ResourcePackInfo> getAppliedResourcePacks() {
        return List.of();
    }

    @Override
    public Collection<ResourcePackInfo> getPendingResourcePacks() {
        return List.of();
    }

    @Override
    public boolean sendPluginMessage(ChannelIdentifier identifier, byte[] data) {
        return Natives.send(name, identifier.getId(), data, false);
    }

    @Override
    public boolean sendPluginMessage(ChannelIdentifier identifier, PluginMessageEncoder encoder) {
        final ByteArrayDataOutput out = ByteStreams.newDataOutput();
        encoder.encode(out);
        return sendPluginMessage(identifier, out.toByteArray());
    }

    @Override
    public String getClientBrand() {
        return "vanilla";
    }

    @Override
    public void addCustomChatCompletions(Collection<String> completions) {
        throw missing("chat completions");
    }

    @Override
    public void removeCustomChatCompletions(Collection<String> completions) {
        throw missing("chat completions");
    }

    @Override
    public void setCustomChatCompletions(Collection<String> completions) {
        throw missing("chat completions");
    }

    @Override
    public void transferToHost(InetSocketAddress target) {
        Hud.send(name, Hud.TRANSFER, target.getHostString(), null, target.getPort(), 0, 0, 0, null);
    }

    @Override
    public void storeCookie(net.kyori.adventure.key.Key key, byte[] data) {
        Hud.send(name, Hud.STORE_COOKIE, key.asString(), null, 0, 0, 0, 0, data);
    }

    @Override
    public void requestCookie(net.kyori.adventure.key.Key key) {
        Hud.cookie(name, key.asString()).thenAccept(data -> BasaltVelocity.proxy().getEventManager()
                .fire(new com.velocitypowered.api.event.player.CookieReceiveEvent(this, key, data)));
    }

    @Override
    public void setServerLinks(List<ServerLink> links) {
        throw missing("server links");
    }

    @Override
    public InetSocketAddress getRemoteAddress() {
        return new InetSocketAddress(address, port);
    }

    @Override
    public Optional<InetSocketAddress> getVirtualHost() {
        return Optional.empty();
    }

    @Override
    public Optional<String> getRawVirtualHost() {
        return Optional.empty();
    }

    @Override
    public boolean isActive() {
        return true;
    }

    @Override
    public ProtocolVersion getProtocolVersion() {
        return ProtocolVersion.MAXIMUM_VERSION;
    }

    @Override
    public ProtocolState getProtocolState() {
        return ProtocolState.PLAY;
    }

    @Override
    public HandshakeIntent getHandshakeIntent() {
        return HandshakeIntent.LOGIN;
    }

    @Override
    public UUID getSessionId() {
        return session;
    }

    @Override
    public Tristate getPermissionValue(String permission) {
        return Natives.permission(name, permission) ? Tristate.TRUE : Tristate.UNDEFINED;
    }

    @Override
    public com.velocitypowered.api.proxy.crypto.IdentifiedKey getIdentifiedKey() {
        return null;
    }

    @Override
    public Identity identity() {
        return Identity.identity(uuid);
    }

    @Override
    public void sendMessage(Component message) {
        Natives.tell(name, Text.json(message));
    }
}
