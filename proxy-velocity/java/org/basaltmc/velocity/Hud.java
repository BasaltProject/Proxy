// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import net.kyori.adventure.bossbar.BossBar;
import net.kyori.adventure.text.Component;

/** What a player sees on screen, handed to the native side already serialized as component JSON. */
final class Hud {
    static final int TITLE = 0;
    static final int CLEAR_TITLE = 1;
    static final int ACTION_BAR = 2;
    static final int TAB = 3;
    static final int BOSS_ADD = 4;
    static final int BOSS_REMOVE = 5;
    static final int BOSS_PROGRESS = 6;
    static final int BOSS_NAME = 7;
    static final int BOSS_STYLE = 8;
    static final int TRANSFER = 9;
    static final int STORE_COOKIE = 10;
    static final int TAB_ENTRY = 13;
    static final int TAB_ENTRY_REMOVE = 14;

    private static final long COOKIE_WAIT_MILLIS = 5000;
    private static final ScheduledExecutorService POLL = Executors.newSingleThreadScheduledExecutor(task -> {
        Thread thread = new Thread(task, "basalt-cookie-poll");
        thread.setDaemon(true);
        return thread;
    });

    private Hud() {}

    static boolean send(String player, int kind, String first, String second, int x, int y, int z, float number, byte[] data) {
        return Natives.hud(player, kind, first, second, x, y, z, number, data);
    }

    static String json(Component component) {
        return component == null ? null : Text.json(component);
    }

    static int color(BossBar.Color color) {
        switch (color) {
            case PINK: return 0;
            case BLUE: return 1;
            case RED: return 2;
            case GREEN: return 3;
            case YELLOW: return 4;
            case WHITE: return 6;
            default: return 5;
        }
    }

    static int overlay(BossBar.Overlay overlay) {
        switch (overlay) {
            case NOTCHED_6: return 1;
            case NOTCHED_10: return 2;
            case NOTCHED_12: return 3;
            case NOTCHED_20: return 4;
            default: return 0;
        }
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
