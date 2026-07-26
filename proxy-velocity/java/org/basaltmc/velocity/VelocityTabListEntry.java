// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.velocitypowered.api.proxy.player.ChatSession;
import com.velocitypowered.api.proxy.player.TabList;
import com.velocitypowered.api.proxy.player.TabListEntry;
import com.velocitypowered.api.util.GameProfile;
import java.nio.charset.StandardCharsets;
import java.util.Optional;
import net.kyori.adventure.text.Component;

/** A row the proxy holds in a player's tab list; every change goes to the client at once. */
final class VelocityTabListEntry implements TabListEntry {
    private final VelocityTabList list;
    private final GameProfile profile;
    private Component displayName;
    private int latency;
    private int gameMode;
    private boolean listed = true;

    VelocityTabListEntry(VelocityTabList list, GameProfile profile, Component displayName, int latency, int gameMode) {
        this.list = list;
        this.profile = profile;
        this.displayName = displayName;
        this.latency = latency;
        this.gameMode = gameMode;
    }

    void send() {
        String shown = displayName != null ? Hud.json(displayName) : Hud.json(Component.text(profile.getName()));
        Hud.send(list.player(), Hud.TAB_ENTRY, profile.getId().toString(), shown, latency, 0, 0, 0,
                profile.getName().getBytes(StandardCharsets.UTF_8));
    }

    @Override
    public ChatSession getChatSession() {
        return null;
    }

    @Override
    public TabList getTabList() {
        return list;
    }

    @Override
    public GameProfile getProfile() {
        return profile;
    }

    @Override
    public Optional<Component> getDisplayNameComponent() {
        return Optional.ofNullable(displayName);
    }

    @Override
    public TabListEntry setDisplayName(Component name) {
        this.displayName = name;
        send();
        return this;
    }

    @Override
    public int getLatency() {
        return latency;
    }

    @Override
    public TabListEntry setLatency(int value) {
        this.latency = value;
        send();
        return this;
    }

    @Override
    public int getGameMode() {
        return gameMode;
    }

    @Override
    public TabListEntry setGameMode(int value) {
        this.gameMode = value;
        return this;
    }

    @Override
    public boolean isListed() {
        return listed;
    }

    @Override
    public TabListEntry setListed(boolean value) {
        this.listed = value;
        return this;
    }
}
