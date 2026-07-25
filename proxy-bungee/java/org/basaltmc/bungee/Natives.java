// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

final class Natives {
    private Natives() {}

    static native void log(String line);

    static native boolean connect(String player, String server);

    static native void kick(String player, String json);

    static native void tell(String player, String json);

    static native boolean send(String player, String channel, byte[] data, boolean toBackend);

    static native void channel(String name, boolean add);

    /** Draws on a player's screen; kind and the other arguments are the native side's table. */
    static native boolean hud(String player, int kind, String first, String second, int x, int y, int z, float number, byte[] data);

    /** Whether the proxy's own groups grant the player the node. */
    static native boolean permission(String player, String node);

    /** Has the player say a line, or run it as a command when it starts with a slash. */
    static native boolean spoofChat(String player, String line);

    /** The name of the proxy group the player is in. */
    static native String group(String player);

    /** Puts the player in a group of groups.toml, or in none when empty; false when that is not possible. */
    static native boolean setGroup(String player, String group);

    /** Overrides a node for the player until the proxy stops: 1 grants, 0 denies, negative removes. */
    static native void setPermission(String player, String node, int value);

    /** A token for a cookie asked of the client, or 0 when it cannot be asked. */
    static native long cookieRequest(String player, String key);

    /** Null while waiting, otherwise one element: the cookie, or null when the client has none. A negative token forgets it. */
    static native byte[][] cookiePoll(long token);

    /** bind, port, online mode, compression, connect timeout, read timeout, max sessions, anti proxy, default route, forced hosts. */
    static native String[] config();

    static native String[] servers();

    static native String serverAddress(String name);

    /** Each row: name, uuid, server, address, port, ping. */
    static native String[][] players();
}
