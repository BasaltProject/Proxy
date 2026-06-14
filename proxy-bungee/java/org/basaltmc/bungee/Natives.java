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

    /** bind, port, online mode, compression, connect timeout, read timeout, max sessions, anti proxy, default route, forced hosts. */
    static native String[] config();

    static native String[] servers();

    static native String serverAddress(String name);

    /** Each row: name, uuid, server, address, port, ping. */
    static native String[][] players();
}
