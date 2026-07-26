// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.velocitypowered.api.proxy.crypto.IdentifiedKey;
import com.velocitypowered.api.proxy.player.ChatSession;
import com.velocitypowered.api.proxy.player.TabList;
import com.velocitypowered.api.proxy.player.TabListEntry;
import com.velocitypowered.api.util.GameProfile;
import java.util.Collection;
import java.util.List;
import java.util.Optional;
import java.util.UUID;
import net.kyori.adventure.text.Component;

/** The header and footer of a player's tab list, and the rows the proxy adds to it. */
final class VelocityTabList implements TabList {
    private final String player;
    private final java.util.Map<UUID, VelocityTabListEntry> entries = new java.util.concurrent.ConcurrentHashMap<>();

    VelocityTabList(String player) {
        this.player = player;
    }

    @Override
    public void setHeaderAndFooter(Component header, Component footer) {
        Hud.send(player, Hud.TAB, Hud.json(header), Hud.json(footer), 0, 0, 0, 0, null);
    }

    @Override
    public void clearHeaderAndFooter() {
        setHeaderAndFooter(Component.empty(), Component.empty());
    }

    String player() {
        return player;
    }

    @Override
    public void addEntry(TabListEntry entry) {
        VelocityTabListEntry mine = entry instanceof VelocityTabListEntry known ? known
                : new VelocityTabListEntry(this, entry.getProfile(), entry.getDisplayNameComponent().orElse(null), entry.getLatency(),
                        entry.getGameMode());
        entries.put(entry.getProfile().getId(), mine);
        mine.send();
    }

    @Override
    public Optional<TabListEntry> removeEntry(UUID uuid) {
        VelocityTabListEntry removed = entries.remove(uuid);
        if (removed != null) {
            Hud.send(player, Hud.TAB_ENTRY_REMOVE, uuid.toString(), null, 0, 0, 0, 0, null);
        }
        return Optional.ofNullable(removed);
    }

    @Override
    public boolean containsEntry(UUID uuid) {
        return entries.containsKey(uuid);
    }

    @Override
    public Optional<TabListEntry> getEntry(UUID uuid) {
        return Optional.ofNullable(entries.get(uuid));
    }

    @Override
    public Collection<TabListEntry> getEntries() {
        return List.copyOf(entries.values());
    }

    @Override
    public void clearAll() {
        for (UUID id : List.copyOf(entries.keySet())) {
            removeEntry(id);
        }
    }

    @Override
    public TabListEntry buildEntry(GameProfile profile, Component displayName, int latency, int gameMode, ChatSession session,
            boolean listed, int listOrder, boolean showHat) {
        return new VelocityTabListEntry(this, profile, displayName, latency, gameMode);
    }
}
