// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.velocitypowered.api.event.player.PlayerResourcePackStatusEvent;
import com.velocitypowered.api.proxy.player.ResourcePackInfo;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import net.kyori.adventure.resource.ResourcePackCallback;
import net.kyori.adventure.resource.ResourcePackRequest;
import net.kyori.adventure.resource.ResourcePackStatus;

/** The resource packs plugins offered one player, as the client reported them back. */
final class PlayerPacks {
    private final VelocityPlayer player;
    private final Map<UUID, ResourcePackInfo> pending = new ConcurrentHashMap<>();
    private final Map<UUID, ResourcePackInfo> applied = new ConcurrentHashMap<>();
    private final Map<UUID, ResourcePackCallback> callbacks = new ConcurrentHashMap<>();

    PlayerPacks(VelocityPlayer player) {
        this.player = player;
    }

    void offer(ResourcePackInfo info) {
        final VelocityPack pack = info instanceof VelocityPack mine ? mine
                : new VelocityPack(info.getId(), info.getUrl(), info.getHash(), info.getShouldForce(), info.getPrompt(),
                        ResourcePackInfo.Origin.PLUGIN_ON_PROXY);
        if (pack.send(player.getUsername())) {
            applied.remove(pack.getId());
            pending.put(pack.getId(), pack);
        }
    }

    void offer(ResourcePackRequest request) {
        if (request.replace()) {
            clear();
        }
        for (final net.kyori.adventure.resource.ResourcePackInfo info : request.packs()) {
            callbacks.put(info.id(), request.callback());
            offer(VelocityPack.of(info, request.required(), request.prompt()));
        }
    }

    void remove(UUID id) {
        if (Hud.send(player.getUsername(), Hud.PACK_POP, id.toString(), null, 0, 0, 0, 0, null)) {
            pending.remove(id);
            applied.remove(id);
        }
    }

    void clear() {
        Hud.send(player.getUsername(), Hud.PACK_CLEAR, null, null, 0, 0, 0, 0, null);
        pending.clear();
        applied.clear();
    }

    Collection<ResourcePackInfo> pending() {
        return new ArrayList<>(pending.values());
    }

    Collection<ResourcePackInfo> applied() {
        return new ArrayList<>(applied.values());
    }

    PlayerResourcePackStatusEvent status(UUID id, int code) {
        final PlayerResourcePackStatusEvent.Status[] all = PlayerResourcePackStatusEvent.Status.values();
        final PlayerResourcePackStatusEvent.Status status = all[Math.max(0, Math.min(all.length - 1, code))];
        final ResourcePackInfo info = pending.containsKey(id) ? pending.get(id) : applied.get(id);
        if (status == PlayerResourcePackStatusEvent.Status.SUCCESSFUL && info != null) {
            pending.remove(id);
            applied.put(id, info);
        } else if (!status.isIntermediate()) {
            pending.remove(id);
            applied.remove(id);
        }
        final ResourcePackCallback callback = status.isIntermediate() ? callbacks.get(id) : callbacks.remove(id);
        if (callback != null) {
            callback.packEventReceived(id, status == PlayerResourcePackStatusEvent.Status.SUCCESSFUL
                    ? ResourcePackStatus.SUCCESSFULLY_LOADED : ResourcePackStatus.valueOf(status.name()), player);
        }
        return new PlayerResourcePackStatusEvent(player, id, status, info);
    }
}
