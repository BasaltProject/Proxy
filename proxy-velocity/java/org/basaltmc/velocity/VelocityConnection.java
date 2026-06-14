// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.google.common.io.ByteArrayDataOutput;
import com.google.common.io.ByteStreams;
import com.velocitypowered.api.proxy.Player;
import com.velocitypowered.api.proxy.ServerConnection;
import com.velocitypowered.api.proxy.messages.ChannelIdentifier;
import com.velocitypowered.api.proxy.messages.PluginMessageEncoder;
import com.velocitypowered.api.proxy.server.RegisteredServer;
import com.velocitypowered.api.proxy.server.ServerInfo;
import java.util.Optional;

final class VelocityConnection implements ServerConnection {
    private final VelocityPlayer player;
    private final RegisteredServer server;

    VelocityConnection(VelocityPlayer player, RegisteredServer server) {
        this.player = player;
        this.server = server;
    }

    @Override
    public RegisteredServer getServer() {
        return server;
    }

    @Override
    public Optional<RegisteredServer> getPreviousServer() {
        return Optional.empty();
    }

    @Override
    public ServerInfo getServerInfo() {
        return server.getServerInfo();
    }

    @Override
    public boolean isClientLoaded() {
        return true;
    }

    @Override
    public Player getPlayer() {
        return player;
    }

    @Override
    public boolean sendPluginMessage(ChannelIdentifier identifier, byte[] data) {
        return Natives.send(player.getUsername(), identifier.getId(), data, true);
    }

    @Override
    public boolean sendPluginMessage(ChannelIdentifier identifier, PluginMessageEncoder encoder) {
        final ByteArrayDataOutput out = ByteStreams.newDataOutput();
        encoder.encode(out);
        return sendPluginMessage(identifier, out.toByteArray());
    }
}
