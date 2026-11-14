// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.util.ArrayList;
import java.util.Collection;
import net.md_5.bungee.api.Callback;
import net.md_5.bungee.api.CommandSender;
import net.md_5.bungee.api.ServerPing;
import net.md_5.bungee.api.config.ServerInfo;
import net.md_5.bungee.api.connection.ProxiedPlayer;

final class BasaltServerInfo implements ServerInfo {

    private final String name;
    private final InetSocketAddress declared;

    BasaltServerInfo(String name) {
        this(name, null);
    }

    BasaltServerInfo(String name, InetSocketAddress declared) {
        this.name = name;
        this.declared = declared;
    }

    @Override
    public String getName() {
        return name;
    }

    @Override
    public InetSocketAddress getAddress() {
        final String text = Natives.serverAddress(name);
        if (text == null) {
            return declared != null ? declared : InetSocketAddress.createUnresolved("localhost", 0);
        }
        final int colon = text.lastIndexOf(':');
        return InetSocketAddress.createUnresolved(text.substring(0, colon), Integer.parseInt(text.substring(colon + 1)));
    }

    @Override
    public SocketAddress getSocketAddress() {
        return getAddress();
    }

    @Override
    public Collection<ProxiedPlayer> getPlayers() {
        final Collection<ProxiedPlayer> result = new ArrayList<>();
        for (final ProxiedPlayer player : BasaltBungee.proxy().getPlayers()) {
            if (player.getServer() != null && name.equals(player.getServer().getInfo().getName())) {
                result.add(player);
            }
        }
        return result;
    }

    @Override
    public String getMotd() {
        return "";
    }

    @Override
    public boolean isRestricted() {
        return false;
    }

    @Override
    public String getPermission() {
        return "bungeecord.server." + name;
    }

    @Override
    public boolean canAccess(CommandSender sender) {
        return true;
    }

    @Override
    public void sendData(String channel, byte[] data) {
        sendData(channel, data, true);
    }

    @Override
    public boolean sendData(String channel, byte[] data, boolean queue) {
        for (final ProxiedPlayer player : getPlayers()) {
            return Natives.send(player.getName(), channel, data, true);
        }
        return false;
    }

    @Override
    public void ping(Callback<ServerPing> callback) {
        Hud.ping(name).whenComplete((f, error) -> {
            if (error != null) {
                callback.done(null, error instanceof java.util.concurrent.CompletionException ? error.getCause() : error);
                return;
            }
            final int online = Integer.parseInt(f[1]);
            callback.done(new ServerPing(new ServerPing.Protocol(f[3], Integer.parseInt(f[4])),
                    online < 0 ? null : new ServerPing.Players(Integer.parseInt(f[2]), online, null),
                    net.md_5.bungee.chat.ComponentSerializer.deserialize(f[0]), (net.md_5.bungee.api.Favicon) null), null);
        });
    }
}
