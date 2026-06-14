// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.velocitypowered.api.network.HandshakeIntent;
import com.velocitypowered.api.network.ProtocolState;
import com.velocitypowered.api.network.ProtocolVersion;
import com.velocitypowered.api.proxy.InboundConnection;
import java.net.InetSocketAddress;
import java.util.Optional;
import java.util.UUID;

/** A connection that has not become a player yet, or a status ping. */
final class VelocityPending implements InboundConnection {
    private final String address;
    private final int port;
    private final String host;
    private final boolean status;
    private final UUID session = UUID.randomUUID();

    VelocityPending(String address, int port, String host, boolean status) {
        this.address = address;
        this.port = port;
        this.host = host;
        this.status = status;
    }

    @Override
    public InetSocketAddress getRemoteAddress() {
        return new InetSocketAddress(address, port);
    }

    @Override
    public Optional<InetSocketAddress> getVirtualHost() {
        return host == null || host.isEmpty() ? Optional.empty() : Optional.of(InetSocketAddress.createUnresolved(host, 25565));
    }

    @Override
    public Optional<String> getRawVirtualHost() {
        return Optional.ofNullable(host);
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
        return status ? ProtocolState.STATUS : ProtocolState.LOGIN;
    }

    @Override
    public HandshakeIntent getHandshakeIntent() {
        return status ? HandshakeIntent.STATUS : HandshakeIntent.LOGIN;
    }

    @Override
    public UUID getSessionId() {
        return session;
    }
}
