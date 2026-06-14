// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.velocitypowered.api.event.Continuation;
import com.velocitypowered.api.event.EventHandler;
import com.velocitypowered.api.event.EventManager;
import com.velocitypowered.api.event.EventTask;
import com.velocitypowered.api.event.PostOrder;
import com.velocitypowered.api.event.Subscribe;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

final class VelocityEvents implements EventManager {
    private static final long WAIT_MILLIS = 2500;

    private record Registration(Object plugin, Object listener, Class<?> type, short priority, EventHandler<Object> handler) {}

    private final List<Registration> registrations = new CopyOnWriteArrayList<>();

    static short priorityOf(PostOrder order, short custom) {
        switch (order) {
            case FIRST:
                return -128;
            case EARLY:
                return -64;
            case LATE:
                return 64;
            case LAST:
                return 127;
            case CUSTOM:
                return custom;
            default:
                return 0;
        }
    }

    @Override
    public void register(Object plugin, Object listener) {
        for (final Method method : listener.getClass().getDeclaredMethods()) {
            final Subscribe subscribe = method.getAnnotation(Subscribe.class);
            if (subscribe == null || method.getParameterCount() < 1) {
                continue;
            }
            method.setAccessible(true);
            final boolean withContinuation = method.getParameterCount() == 2 && method.getParameterTypes()[1] == Continuation.class;
            final EventHandler<Object> handler = new EventHandler<>() {
                @Override
                public void execute(Object event) {
                    invoke(method, listener, event, null);
                }

                @Override
                public EventTask executeAsync(Object event) {
                    if (withContinuation) {
                        return EventTask.withContinuation(continuation -> invoke(method, listener, event, continuation));
                    }
                    final Object result = invoke(method, listener, event, null);
                    return result instanceof EventTask task ? task : null;
                }
            };
            registrations.add(new Registration(plugin, listener, method.getParameterTypes()[0],
                    priorityOf(subscribe.order(), subscribe.priority()), handler));
        }
    }

    private static Object invoke(Method method, Object listener, Object event, Continuation continuation) {
        try {
            return continuation == null ? method.invoke(listener, event) : method.invoke(listener, event, continuation);
        } catch (final java.lang.reflect.InvocationTargetException error) {
            Natives.log("an event handler failed: " + error.getCause());
        } catch (final ReflectiveOperationException error) {
            Natives.log("an event handler could not run: " + error);
        }
        return null;
    }

    @Override
    public <E> void register(Object plugin, Class<E> type, PostOrder order, EventHandler<E> handler) {
        registrations.add(new Registration(plugin, null, type, priorityOf(order, (short) 0), (EventHandler<Object>) handler));
    }

    @Override
    public <E> void register(Object plugin, Class<E> type, short priority, EventHandler<E> handler) {
        registrations.add(new Registration(plugin, null, type, priority, (EventHandler<Object>) handler));
    }

    @Override
    public <E> CompletableFuture<E> fire(E event) {
        final List<Registration> matching = new ArrayList<>();
        for (final Registration registration : registrations) {
            if (registration.type().isInstance(event)) {
                matching.add(registration);
            }
        }
        matching.sort(Comparator.comparingInt(Registration::priority));
        for (final Registration registration : matching) {
            final EventTask task;
            try {
                task = registration.handler().executeAsync(event);
            } catch (final Throwable error) {
                Natives.log("an event handler failed: " + error);
                continue;
            }
            if (task != null) {
                wait(task);
            }
        }
        return CompletableFuture.completedFuture(event);
    }

    private static void wait(EventTask task) {
        final CountDownLatch done = new CountDownLatch(1);
        final AtomicReference<Throwable> failure = new AtomicReference<>();
        final Continuation continuation = new Continuation() {
            @Override
            public void resume() {
                done.countDown();
            }

            @Override
            public void resumeWithException(Throwable error) {
                failure.set(error);
                done.countDown();
            }
        };
        try {
            if (task.requiresAsync()) {
                final Thread thread = new Thread(() -> task.execute(continuation), "Basalt Velocity event");
                thread.setDaemon(true);
                thread.start();
            } else {
                task.execute(continuation);
            }
            if (!done.await(WAIT_MILLIS, TimeUnit.MILLISECONDS)) {
                Natives.log("an event handler did not finish in time");
            }
        } catch (final Throwable error) {
            Natives.log("an event task failed: " + error);
        }
        if (failure.get() != null) {
            Natives.log("an event task failed: " + failure.get());
        }
    }

    @Override
    public void unregisterListeners(Object plugin) {
        registrations.removeIf(registration -> registration.plugin() == plugin);
    }

    @Override
    public void unregisterListener(Object plugin, Object listener) {
        registrations.removeIf(registration -> registration.plugin() == plugin && registration.listener() == listener);
    }

    @Override
    public <E> void unregister(Object plugin, EventHandler<E> handler) {
        registrations.removeIf(registration -> registration.plugin() == plugin && registration.handler() == handler);
    }
}
