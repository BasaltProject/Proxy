// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.velocitypowered.api.scheduler.ScheduledTask;
import com.velocitypowered.api.scheduler.Scheduler;
import com.velocitypowered.api.scheduler.TaskStatus;
import java.util.ArrayList;
import java.util.Collection;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

final class VelocityScheduler implements Scheduler {
    private final ScheduledExecutorService pool = Executors.newScheduledThreadPool(2, runnable -> {
        final Thread thread = new Thread(runnable, "Basalt Velocity scheduler");
        thread.setDaemon(true);
        return thread;
    });
    private final java.util.Set<Task> tasks = ConcurrentHashMap.newKeySet();

    private final class Task implements ScheduledTask, Runnable {
        final Object plugin;
        final Consumer<ScheduledTask> body;
        long delay;
        long repeat;
        TimeUnit delayUnit = TimeUnit.MILLISECONDS;
        TimeUnit repeatUnit = TimeUnit.MILLISECONDS;
        volatile ScheduledFuture<?> future;
        volatile TaskStatus status = TaskStatus.SCHEDULED;

        Task(Object plugin, Consumer<ScheduledTask> body) {
            this.plugin = plugin;
            this.body = body;
        }

        @Override
        public void run() {
            try {
                body.accept(this);
            } catch (final Throwable error) {
                Natives.log("a scheduled task failed: " + error);
            } finally {
                if (repeat <= 0 && status == TaskStatus.SCHEDULED) {
                    status = TaskStatus.FINISHED;
                    tasks.remove(this);
                }
            }
        }

        @Override
        public Object plugin() {
            return plugin;
        }

        @Override
        public TaskStatus status() {
            return status;
        }

        @Override
        public void cancel() {
            status = TaskStatus.CANCELLED;
            if (future != null) {
                future.cancel(false);
            }
            tasks.remove(this);
        }
    }

    private final class Builder implements TaskBuilder {
        private final Task task;

        Builder(Task task) {
            this.task = task;
        }

        @Override
        public TaskBuilder delay(long amount, TimeUnit unit) {
            task.delay = amount;
            task.delayUnit = unit;
            return this;
        }

        @Override
        public TaskBuilder repeat(long amount, TimeUnit unit) {
            task.repeat = amount;
            task.repeatUnit = unit;
            return this;
        }

        @Override
        public TaskBuilder clearDelay() {
            task.delay = 0;
            return this;
        }

        @Override
        public TaskBuilder clearRepeat() {
            task.repeat = 0;
            return this;
        }

        @Override
        public ScheduledTask schedule() {
            tasks.add(task);
            task.future = task.repeat > 0
                    ? pool.scheduleAtFixedRate(task, task.delayUnit.toMillis(task.delay), task.repeatUnit.toMillis(task.repeat), TimeUnit.MILLISECONDS)
                    : pool.schedule(task, task.delayUnit.toMillis(task.delay), TimeUnit.MILLISECONDS);
            return task;
        }
    }

    @Override
    public TaskBuilder buildTask(Object plugin, Runnable body) {
        return new Builder(new Task(plugin, self -> body.run()));
    }

    @Override
    public TaskBuilder buildTask(Object plugin, Consumer<ScheduledTask> body) {
        return new Builder(new Task(plugin, body));
    }

    @Override
    public Collection<ScheduledTask> tasksByPlugin(Object plugin) {
        final Collection<ScheduledTask> result = new ArrayList<>();
        for (final Task task : tasks) {
            if (task.plugin == plugin) {
                result.add(task);
            }
        }
        return result;
    }

    void cancelAll() {
        for (final Task task : tasks) {
            task.cancel();
        }
        pool.shutdownNow();
    }
}
