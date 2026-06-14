// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import net.md_5.bungee.api.chat.BaseComponent;
import net.md_5.bungee.api.chat.TextComponent;
import net.md_5.bungee.api.config.ListenerInfo;
import net.md_5.bungee.api.connection.Connection;
import net.md_5.bungee.api.connection.PendingConnection;

final class BasaltPending implements PendingConnection {
    private final String name;
    private UUID uuid;
    private final String address;
    private final int port;
    private final String host;
    private boolean online = true;

    BasaltPending(String name, String uuidText, String address, int port, String host) {
        this.name = name;
        this.uuid = uuidText == null || uuidText.isEmpty() ? new UUID(0, 0) : UUID.fromString(dashed(uuidText));
        this.address = address;
        this.port = port;
        this.host = host;
    }

    private static String dashed(String text) {
        if (text.indexOf('-') >= 0) {
            return text;
        }
        return text.substring(0, 8) + "-" + text.substring(8, 12) + "-" + text.substring(12, 16) + "-"
                + text.substring(16, 20) + "-" + text.substring(20);
    }

    @Override
    public String getName() {
        return name;
    }

    @Override
    public int getVersion() {
        return 777;
    }

    @Override
    public InetSocketAddress getVirtualHost() {
        return InetSocketAddress.createUnresolved(host, 25565);
    }

    @Override
    public ListenerInfo getListener() {
        return BasaltBungee.proxy().getConfig().getListeners().iterator().next();
    }

    @Override
    public String getUUID() {
        return uuid.toString().replace("-", "");
    }

    @Override
    public UUID getUniqueId() {
        return uuid;
    }

    @Override
    public void setUniqueId(UUID id) {
        uuid = id;
    }

    @Override
    public boolean isOnlineMode() {
        return online;
    }

    @Override
    public void setOnlineMode(boolean value) {
        online = value;
    }

    @Override
    public boolean isLegacy() {
        return false;
    }

    @Override
    public boolean isTransferred() {
        return false;
    }

    @Override
    public CompletableFuture<byte[]> retrieveCookie(String cookie) {
        return CompletableFuture.failedFuture(new UnsupportedOperationException("cookies are not available yet"));
    }

    @Override
    public CompletableFuture<byte[]> sendData(String channel, byte[] data) {
        return CompletableFuture.failedFuture(new UnsupportedOperationException("login channels are not available yet"));
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
        Natives.kick(name, net.md_5.bungee.chat.ComponentSerializer.toString(TextComponent.fromLegacy(reason)));
    }

    @Override
    public void disconnect(BaseComponent... reason) {
        Natives.kick(name, net.md_5.bungee.chat.ComponentSerializer.toString(reason));
    }

    @Override
    public void disconnect(BaseComponent reason) {
        Natives.kick(name, net.md_5.bungee.chat.ComponentSerializer.toString(reason));
    }

    @Override
    public boolean isConnected() {
        return true;
    }

    @Override
    public Connection.Unsafe unsafe() {
        throw new UnsupportedOperationException("raw packets are not available");
    }
}
