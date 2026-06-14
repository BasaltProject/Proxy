// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import net.md_5.bungee.api.plugin.Plugin;
import net.md_5.bungee.api.scheduler.ScheduledTask;
import net.md_5.bungee.api.scheduler.TaskScheduler;

final class BasaltScheduler implements TaskScheduler {
    private final AtomicInteger ids = new AtomicInteger();
    private final ScheduledExecutorService pool = Executors.newScheduledThreadPool(2, runnable -> {
        final Thread thread = new Thread(runnable, "Basalt Bungee scheduler");
        thread.setDaemon(true);
        return thread;
    });
    private final Map<Integer, Task> tasks = new ConcurrentHashMap<>();

    private final class Task implements ScheduledTask {
        final int id = ids.incrementAndGet();
        final Plugin owner;
        final Runnable body;
        volatile ScheduledFuture<?> future;

        Task(Plugin owner, Runnable body) {
            this.owner = owner;
            this.body = body;
        }

        @Override
        public int getId() {
            return id;
        }

        @Override
        public Plugin getOwner() {
            return owner;
        }

        @Override
        public Runnable getTask() {
            return body;
        }

        @Override
        public void cancel() {
            if (future != null) {
                future.cancel(false);
            }
            tasks.remove(id);
        }
    }

    private Runnable guarded(Task task, boolean once) {
        return () -> {
            try {
                task.body.run();
            } catch (final Throwable error) {
                Natives.log("task of " + task.owner.getDescription().getName() + " failed: " + error);
            } finally {
                if (once) {
                    tasks.remove(task.id);
                }
            }
        };
    }

    @Override
    public void cancel(int id) {
        final Task task = tasks.get(id);
        if (task != null) {
            task.cancel();
        }
    }

    @Override
    public void cancel(ScheduledTask task) {
        task.cancel();
    }

    @Override
    public int cancel(Plugin plugin) {
        int count = 0;
        for (final Task task : tasks.values()) {
            if (task.owner == plugin) {
                task.cancel();
                count++;
            }
        }
        return count;
    }

    @Override
    public ScheduledTask runAsync(Plugin owner, Runnable body) {
        return schedule(owner, body, 0, TimeUnit.MILLISECONDS);
    }

    @Override
    public ScheduledTask schedule(Plugin owner, Runnable body, long delay, TimeUnit unit) {
        final Task task = new Task(owner, body);
        tasks.put(task.id, task);
        task.future = pool.schedule(guarded(task, true), delay, unit);
        return task;
    }

    @Override
    public ScheduledTask schedule(Plugin owner, Runnable body, long delay, long period, TimeUnit unit) {
        final Task task = new Task(owner, body);
        tasks.put(task.id, task);
        task.future = pool.scheduleAtFixedRate(guarded(task, false), delay, Math.max(1, period), unit);
        return task;
    }

    void stop() {
        pool.shutdownNow();
    }

    @Override
    public Unsafe unsafe() {
        return plugin -> (ExecutorService) pool;
    }
}
