// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.google.common.io.ByteArrayDataOutput;
import com.google.common.io.ByteStreams;
import com.velocitypowered.api.proxy.Player;
import com.velocitypowered.api.proxy.messages.ChannelIdentifier;
import com.velocitypowered.api.proxy.messages.PluginMessageEncoder;
import com.velocitypowered.api.proxy.server.RegisteredServer;
import com.velocitypowered.api.proxy.server.ServerInfo;
import com.velocitypowered.api.proxy.server.ServerPing;
import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.Collection;
import java.util.concurrent.CompletableFuture;
import net.kyori.adventure.text.Component;

final class VelocityServer implements RegisteredServer {
    private final ServerInfo info;

    VelocityServer(String name) {
        final String text = Natives.serverAddress(name);
        InetSocketAddress address = InetSocketAddress.createUnresolved("localhost", 0);
        if (text != null) {
            final int colon = text.lastIndexOf(':');
            address = InetSocketAddress.createUnresolved(text.substring(0, colon), Integer.parseInt(text.substring(colon + 1)));
        }
        this.info = new ServerInfo(name, address);
    }

    @Override
    public ServerInfo getServerInfo() {
        return info;
    }

    @Override
    public Collection<Player> getPlayersConnected() {
        final Collection<Player> result = new ArrayList<>();
        for (final Player player : BasaltVelocity.proxy().getAllPlayers()) {
            if (player.getCurrentServer().isPresent() && player.getCurrentServer().get().getServerInfo().getName().equals(info.getName())) {
                result.add(player);
            }
        }
        return result;
    }

    @Override
    public CompletableFuture<ServerPing> ping() {
        return CompletableFuture.failedFuture(new UnsupportedOperationException("server pings are not available"));
    }

    @Override
    public CompletableFuture<ServerPing> ping(com.velocitypowered.api.proxy.server.PingOptions options) {
        return ping();
    }

    @Override
    public boolean sendPluginMessage(ChannelIdentifier identifier, byte[] data) {
        for (final Player player : getPlayersConnected()) {
            return Natives.send(player.getUsername(), identifier.getId(), data, true);
        }
        return false;
    }

    @Override
    public boolean sendPluginMessage(ChannelIdentifier identifier, PluginMessageEncoder encoder) {
        final ByteArrayDataOutput out = ByteStreams.newDataOutput();
        encoder.encode(out);
        return sendPluginMessage(identifier, out.toByteArray());
    }

    @Override
    public void sendMessage(Component message) {
        for (final Player player : getPlayersConnected()) {
            player.sendMessage(message);
        }
    }
}
