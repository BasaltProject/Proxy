// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import net.md_5.bungee.api.Callback;
import net.md_5.bungee.api.ChatMessageType;
import net.md_5.bungee.api.ServerConnectRequest;
import net.md_5.bungee.api.SkinConfiguration;
import net.md_5.bungee.api.Title;
import net.md_5.bungee.api.chat.BaseComponent;
import net.md_5.bungee.api.chat.ComponentBuilder;
import net.md_5.bungee.api.chat.TextComponent;
import net.md_5.bungee.api.config.ServerInfo;
import net.md_5.bungee.api.connection.Connection;
import net.md_5.bungee.api.connection.PendingConnection;
import net.md_5.bungee.api.connection.ProxiedPlayer;
import net.md_5.bungee.api.connection.Server;
import net.md_5.bungee.api.event.ServerConnectEvent;
import net.md_5.bungee.api.score.Scoreboard;
import net.md_5.bungee.chat.ComponentSerializer;

final class BasaltPlayer implements ProxiedPlayer {
    private final String name;
    private final UUID uuid;
    private final String uuidText;
    private final String address;
    private final int port;
    private volatile String server;
    private volatile int ping;
    private volatile boolean connected = true;
    private String displayName;
    private final BasaltPending pending;

    BasaltPlayer(String name, String uuidText, String server, String address, int port, int ping) {
        this.name = name;
        this.uuidText = uuidText == null ? "" : uuidText.replace("-", "");
        this.uuid = uuidText == null || uuidText.isEmpty() ? new UUID(0, 0) : UUID.fromString(dashed(uuidText));
        this.server = server;
        this.address = address;
        this.port = port;
        this.ping = ping;
        this.displayName = name;
        this.pending = new BasaltPending(name, uuidText, address, port, "");
    }

    private static String dashed(String text) {
        if (text.indexOf('-') >= 0) {
            return text;
        }
        return text.substring(0, 8) + "-" + text.substring(8, 12) + "-" + text.substring(12, 16) + "-"
                + text.substring(16, 20) + "-" + text.substring(20);
    }

    void update(String server, int ping) {
        this.server = server;
        this.ping = ping;
    }

    void gone() {
        connected = false;
    }

    @Override
    public String getName() {
        return name;
    }

    @Override
    public String getDisplayName() {
        return displayName;
    }

    @Override
    public void setDisplayName(String value) {
        displayName = value;
    }

    @Override
    public void sendMessage(ChatMessageType type, BaseComponent... message) {
        sendMessage(message);
    }

    @Override
    public void sendMessage(ChatMessageType type, BaseComponent message) {
        sendMessage(message);
    }

    @Override
    public void sendMessage(UUID sender, BaseComponent... message) {
        sendMessage(message);
    }

    @Override
    public void sendMessage(UUID sender, BaseComponent message) {
        sendMessage(message);
    }

    @Override
    public void sendMessage(String message) {
        sendMessage(TextComponent.fromLegacy(message));
    }

    @Override
    public void sendMessages(String... messages) {
        for (final String message : messages) {
            sendMessage(message);
        }
    }

    @Override
    public void sendMessage(BaseComponent... message) {
        Natives.tell(name, ComponentSerializer.toString(message));
    }

    @Override
    public void sendMessage(BaseComponent message) {
        Natives.tell(name, ComponentSerializer.toString(message));
    }

    private boolean connectTo(ServerInfo target) {
        final ServerInfo chosen = BasaltBungee.preConnect(this, target);
        return chosen != null && Natives.connect(name, chosen.getName());
    }

    @Override
    public void connect(ServerInfo target) {
        connectTo(target);
    }

    @Override
    public void connect(ServerInfo target, ServerConnectEvent.Reason reason) {
        connect(target);
    }

    @Override
    public void connect(ServerInfo target, Callback<Boolean> callback) {
        callback.done(connectTo(target), null);
    }

    @Override
    public void connect(ServerInfo target, Callback<Boolean> callback, ServerConnectEvent.Reason reason) {
        connect(target, callback);
    }

    @Override
    public void connect(ServerConnectRequest request) {
        connect(request.getTarget());
    }

    @Override
    public Server getServer() {
        return server == null || server.isEmpty() ? null : new BasaltServer(this, BasaltBungee.proxy().getServerInfo(server));
    }

    @Override
    public int getPing() {
        return ping;
    }

    @Override
    public void sendData(String channel, byte[] data) {
        Natives.send(name, channel, data, true);
    }

    @Override
    public PendingConnection getPendingConnection() {
        return pending;
    }

    @Override
    public void chat(String message) {
        throw new UnsupportedOperationException("a proxy cannot speak for a player yet");
    }

    @Override
    public ServerInfo getReconnectServer() {
        return null;
    }

    @Override
    public void setReconnectServer(ServerInfo server) {}

    @Override
    public String getUUID() {
        return uuidText;
    }

    @Override
    public UUID getUniqueId() {
        return uuid;
    }

    @Override
    public Locale getLocale() {
        return Locale.ENGLISH;
    }

    @Override
    public byte getViewDistance() {
        return 10;
    }

    @Override
    public ProxiedPlayer.ChatMode getChatMode() {
        return ProxiedPlayer.ChatMode.SHOWN;
    }

    @Override
    public boolean hasChatColors() {
        return true;
    }

    @Override
    public SkinConfiguration getSkinParts() {
        throw new UnsupportedOperationException("skin parts are not tracked");
    }

    @Override
    public ProxiedPlayer.MainHand getMainHand() {
        return ProxiedPlayer.MainHand.RIGHT;
    }

    @Override
    public void setTabHeader(BaseComponent header, BaseComponent footer) {
        throw new UnsupportedOperationException("tab headers are not available yet");
    }

    @Override
    public void setTabHeader(BaseComponent[] header, BaseComponent[] footer) {
        throw new UnsupportedOperationException("tab headers are not available yet");
    }

    @Override
    public void resetTabHeader() {}

    @Override
    public void sendTitle(Title title) {
        throw new UnsupportedOperationException("titles are not available yet");
    }

    @Override
    public boolean isForgeUser() {
        return false;
    }

    @Override
    public Map<String, String> getModList() {
        return Map.of();
    }

    @Override
    public Scoreboard getScoreboard() {
        throw new UnsupportedOperationException("scoreboards are not available yet");
    }

    @Override
    public CompletableFuture<byte[]> retrieveCookie(String cookie) {
        return CompletableFuture.failedFuture(new UnsupportedOperationException("cookies are not available yet"));
    }

    @Override
    public void storeCookie(String cookie, byte[] data) {
        throw new UnsupportedOperationException("cookies are not available yet");
    }

    @Override
    public void transfer(String host, int port) {
        throw new UnsupportedOperationException("transfers are not available yet");
    }

    @Override
    public String getClientBrand() {
        return "vanilla";
    }

    @Override
    public void clearDialog() {}

    @Override
    public void showDialog(net.md_5.bungee.api.dialog.Dialog dialog) {
        throw new UnsupportedOperationException("dialogs are not available yet");
    }

    @Override
    public void sendServerLinks(List<net.md_5.bungee.api.ServerLink> links) {
        throw new UnsupportedOperationException("server links are not available yet");
    }

    @Override
    public InetSocketAddress getAddress() {
        return new InetSocketAddress(address, port);
    }

    @Override
    public SocketAddress getSocketAddress() {
        return getAddress();
    }

    @Override
    public void disconnect(String reason) {
        disconnect(TextComponent.fromLegacy(reason));
    }

    @Override
    public void disconnect(BaseComponent... reason) {
        Natives.kick(name, ComponentSerializer.toString(reason));
    }

    @Override
    public void disconnect(BaseComponent reason) {
        Natives.kick(name, ComponentSerializer.toString(reason));
    }

    @Override
    public boolean isConnected() {
        return connected;
    }

    @Override
    public Connection.Unsafe unsafe() {
        throw new UnsupportedOperationException("raw packets are not available");
    }

    @Override
    public Collection<String> getGroups() {
        return new ArrayList<>();
    }

    @Override
    public void addGroups(String... groups) {}

    @Override
    public void removeGroups(String... groups) {}

    @Override
    public boolean hasPermission(String permission) {
        return false;
    }

    @Override
    public void setPermission(String permission, boolean value) {}

    @Override
    public Collection<String> getPermissions() {
        return new ArrayList<>();
    }
}
