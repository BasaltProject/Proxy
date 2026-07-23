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

/** The header and footer of a player's tab list; entries stay with the backend. */
final class VelocityTabList implements TabList {
    private final String player;

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

    @Override
    public void addEntry(TabListEntry entry) {
        throw new UnsupportedOperationException("tab list entries belong to the backend");
    }

    @Override
    public Optional<TabListEntry> removeEntry(UUID uuid) {
        return Optional.empty();
    }

    @Override
    public boolean containsEntry(UUID uuid) {
        return false;
    }

    @Override
    public Optional<TabListEntry> getEntry(UUID uuid) {
        return Optional.empty();
    }

    @Override
    public Collection<TabListEntry> getEntries() {
        return List.of();
    }

    @Override
    public void clearAll() {}

    @Override
    public TabListEntry buildEntry(GameProfile profile, Component displayName, int latency, int gameMode, ChatSession session,
            boolean listed, int listOrder, boolean showHat) {
        throw new UnsupportedOperationException("tab list entries belong to the backend");
    }
}
