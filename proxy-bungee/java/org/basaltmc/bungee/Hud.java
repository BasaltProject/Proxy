// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import net.md_5.bungee.api.chat.BaseComponent;
import net.md_5.bungee.chat.ComponentSerializer;

/** What a player sees on screen, handed to the native side already serialized as component JSON. */
final class Hud {
    static final int TITLE = 0;
    static final int CLEAR_TITLE = 1;
    static final int ACTION_BAR = 2;
    static final int TAB = 3;
    static final int TRANSFER = 9;
    static final int DIALOG = 15;
    static final int STORE_COOKIE = 10;

    private static final long COOKIE_WAIT_MILLIS = 5000;
    private static final ScheduledExecutorService POLL = Executors.newSingleThreadScheduledExecutor(task -> {
        Thread thread = new Thread(task, "basalt-cookie-poll");
        thread.setDaemon(true);
        return thread;
    });

    private Hud() {}

    static String json(BaseComponent... parts) {
        return parts == null ? null : ComponentSerializer.toString(parts);
    }

    static boolean send(String player, int kind, String first, String second, int x, int y, int z, float number, byte[] data) {
        return Natives.hud(player, kind, first, second, x, y, z, number, data);
    }

    static CompletableFuture<byte[]> cookie(String player, String key) {
        CompletableFuture<byte[]> future = new CompletableFuture<>();
        long token = Natives.cookieRequest(player, key);
        if (token == 0) {
            future.completeExceptionally(new IllegalStateException("cookies need a native 26.3 client in play (translated and legacy clients have none)"));
            return future;
        }
        long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(COOKIE_WAIT_MILLIS);
        POLL.schedule(new Runnable() {
            @Override
            public void run() {
                byte[][] answer = Natives.cookiePoll(token);
                if (answer != null) {
                    future.complete(answer[0]);
                } else if (System.nanoTime() > deadline) {
                    Natives.cookiePoll(-token);
                    future.completeExceptionally(new TimeoutException("the client did not answer the cookie request"));
                } else {
                    POLL.schedule(this, 25, TimeUnit.MILLISECONDS);
                }
            }
        }, 25, TimeUnit.MILLISECONDS);
        return future;
    }
}
