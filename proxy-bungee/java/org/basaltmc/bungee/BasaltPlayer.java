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
        if (type == ChatMessageType.ACTION_BAR) {
            Hud.send(name, Hud.ACTION_BAR, Hud.json(message), null, 0, 0, 0, 0, null);
        } else {
            sendMessage(message);
        }
    }

    @Override
    public void sendMessage(ChatMessageType type, BaseComponent message) {
        sendMessage(type, new BaseComponent[] {message});
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
        Natives.spoofChat(name, message);
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
        setTabHeader(new BaseComponent[] {header}, new BaseComponent[] {footer});
    }

    @Override
    public void setTabHeader(BaseComponent[] header, BaseComponent[] footer) {
        Hud.send(name, Hud.TAB, Hud.json(header), Hud.json(footer), 0, 0, 0, 0, null);
    }

    @Override
    public void resetTabHeader() {
        Hud.send(name, Hud.TAB, Hud.json(new BaseComponent[0]), Hud.json(new BaseComponent[0]), 0, 0, 0, 0, null);
    }

    @Override
    public void sendTitle(Title title) {
        title.send(this);
    }

    @Override
    public boolean isForgeUser() {
        return false;
    }

    @Override
    public Map<String, String> getModList() {
        return Map.of();
    }

    private final Scoreboard scoreboard = new Scoreboard();

    @Override
    public Scoreboard getScoreboard() {
        return scoreboard;
    }

    @Override
    public CompletableFuture<byte[]> retrieveCookie(String cookie) {
        return Hud.cookie(name, cookie);
    }

    @Override
    public void storeCookie(String cookie, byte[] data) {
        Hud.send(name, Hud.STORE_COOKIE, cookie, null, 0, 0, 0, 0, data);
    }

    @Override
    public void transfer(String host, int port) {
        Hud.send(name, Hud.TRANSFER, host, null, port, 0, 0, 0, null);
    }

    @Override
    public String getClientBrand() {
        return "vanilla";
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
    public void showDialog(net.md_5.bungee.api.dialog.Dialog dialog) {
        final net.md_5.bungee.api.dialog.DialogBase base = dialog.getBase();
        final StringBuilder body = new StringBuilder();
        for (net.md_5.bungee.api.dialog.body.DialogBody entry : base.body() == null ? List.<net.md_5.bungee.api.dialog.body.DialogBody>of() : base.body()) {
            if (entry instanceof net.md_5.bungee.api.dialog.body.PlainMessageBody plain) {
                body.append(Hud.json(plain.contents())).append('\n');
            }
        }
        final List<net.md_5.bungee.api.dialog.action.ActionButton> buttons = new ArrayList<>();
        int kind = 0;
        if (dialog instanceof net.md_5.bungee.api.dialog.NoticeDialog notice) {
            if (notice.action() != null) {
                buttons.add(notice.action());
            }
        } else if (dialog instanceof net.md_5.bungee.api.dialog.ConfirmationDialog confirmation) {
            kind = 1;
            buttons.add(confirmation.yes());
            buttons.add(confirmation.no());
        } else if (dialog instanceof net.md_5.bungee.api.dialog.MultiActionDialog multi) {
            kind = 2;
            if (multi.actions() != null) {
                buttons.addAll(multi.actions());
            }
        } else {
            throw new UnsupportedOperationException("only notice, confirmation and multi action dialogs are available");
        }
        final StringBuilder rows = new StringBuilder();
        for (net.md_5.bungee.api.dialog.action.ActionButton button : buttons) {
            final String command = button.action() instanceof net.md_5.bungee.api.dialog.action.RunCommandAction run
                    ? run.template() : "";
            rows.append(Hud.json(button.label())).append('\t').append(command).append('\n');
        }
        final boolean closable = base.canCloseWithEscape() == null || base.canCloseWithEscape();
        Hud.send(name, Hud.DIALOG, Hud.json(base.title()), body.toString(), kind, closable ? 1 : 0, 0, 0,
                rows.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
    }

    @Override
    public void clearDialog() {
        Hud.send(name, Hud.DIALOG, null, null, 3, 1, 0, 0, null);
    }

    @Override
    public boolean isConnected() {
        return connected;
    }

    @Override
    public Connection.Unsafe unsafe() {
        throw new UnsupportedOperationException("BungeeCord packet classes are not 26.3 packets; use packet.send from Lua");
    }

    @Override
    public Collection<String> getGroups() {
        return new ArrayList<>(List.of(Natives.group(name)));
    }

    @Override
    public void addGroups(String... groups) {
        if (groups.length > 0) {
            Natives.setGroup(name, groups[groups.length - 1]);
        }
    }

    @Override
    public void removeGroups(String... groups) {
        if (java.util.Arrays.asList(groups).contains(Natives.group(name))) {
            Natives.setGroup(name, "");
        }
    }

    @Override
    public boolean hasPermission(String permission) {
        return Natives.permission(name, permission);
    }

    @Override
    public void setPermission(String permission, boolean value) {
        Natives.setPermission(name, permission, value ? 1 : 0);
    }

    @Override
    public Collection<String> getPermissions() {
        return new ArrayList<>(List.of(Natives.permissionList(name)));
    }
}
