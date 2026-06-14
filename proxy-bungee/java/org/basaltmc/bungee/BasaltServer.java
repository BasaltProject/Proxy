// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import java.net.InetSocketAddress;
import java.net.SocketAddress;
import net.md_5.bungee.api.chat.BaseComponent;
import net.md_5.bungee.api.config.ServerInfo;
import net.md_5.bungee.api.connection.Connection;
import net.md_5.bungee.api.connection.Server;

final class BasaltServer implements Server {
    private final BasaltPlayer player;
    private final ServerInfo info;

    BasaltServer(BasaltPlayer player, ServerInfo info) {
        this.player = player;
        this.info = info;
    }

    @Override
    public ServerInfo getInfo() {
        return info;
    }

    @Override
    public void sendData(String channel, byte[] data) {
        Natives.send(player.getName(), channel, data, true);
    }

    @Override
    public InetSocketAddress getAddress() {
        return info.getAddress();
    }

    @Override
    public SocketAddress getSocketAddress() {
        return info.getSocketAddress();
    }

    @Override
    public void disconnect(String reason) {
        player.disconnect(reason);
    }

    @Override
    public void disconnect(BaseComponent... reason) {
        player.disconnect(reason);
    }

    @Override
    public void disconnect(BaseComponent reason) {
        player.disconnect(reason);
    }

    @Override
    public boolean isConnected() {
        return player.isConnected();
    }

    @Override
    public Connection.Unsafe unsafe() {
        throw new UnsupportedOperationException("raw packets are not available");
    }
}
