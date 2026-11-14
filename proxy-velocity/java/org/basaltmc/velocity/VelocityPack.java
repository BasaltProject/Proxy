// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.velocitypowered.api.proxy.player.ResourcePackInfo;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.HexFormat;
import java.util.UUID;
import net.kyori.adventure.resource.ResourcePackRequest;
import net.kyori.adventure.text.Component;

/** A resource pack a plugin offers through the proxy. */
final class VelocityPack implements ResourcePackInfo {
    private final UUID id;
    private final String url;
    private final byte[] hash;
    private final boolean force;
    private final Component prompt;
    private final Origin origin;

    VelocityPack(UUID id, String url, byte[] hash, boolean force, Component prompt, Origin origin) {
        this.id = id == null ? UUID.nameUUIDFromBytes(url.getBytes(StandardCharsets.UTF_8)) : id;
        this.url = url;
        this.hash = hash;
        this.force = force;
        this.prompt = prompt;
        this.origin = origin;
    }

    static VelocityPack of(net.kyori.adventure.resource.ResourcePackInfo info, boolean required, Component prompt) {
        final String hash = info.hash();
        return new VelocityPack(info.id(), info.uri().toString(), hash == null || hash.isEmpty() ? null : HexFormat.of().parseHex(hash),
                required, prompt, Origin.PLUGIN_ON_PROXY);
    }

    /** Sends it to the player; false when the proxy refused it. */
    boolean send(String player) {
        final String hex = hash == null ? "" : HexFormat.of().formatHex(hash);
        return Hud.send(player, Hud.PACK_PUSH, id.toString(), prompt == null ? null : Hud.json(prompt), force ? 1 : 0, 0, 0, 0,
                (url + "\n" + hex).getBytes(StandardCharsets.UTF_8));
    }

    @Override
    public UUID getId() {
        return id;
    }

    @Override
    public String getUrl() {
        return url;
    }

    @Override
    public Component getPrompt() {
        return prompt;
    }

    @Override
    public boolean getShouldForce() {
        return force;
    }

    @Override
    public byte[] getHash() {
        return hash == null ? null : hash.clone();
    }

    @Override
    public Origin getOrigin() {
        return origin;
    }

    @Override
    public Origin getOriginalOrigin() {
        return origin;
    }

    @Override
    public ResourcePackInfo.Builder asBuilder() {
        return asBuilder(url);
    }

    @Override
    public ResourcePackInfo.Builder asBuilder(String newUrl) {
        return new Builder(newUrl).setId(id).setShouldForce(force).setHash(hash).setPrompt(prompt);
    }

    @Override
    public ResourcePackRequest asResourcePackRequest() {
        final net.kyori.adventure.resource.ResourcePackInfo info = net.kyori.adventure.resource.ResourcePackInfo.resourcePackInfo(
                id, URI.create(url), hash == null ? "" : HexFormat.of().formatHex(hash));
        return ResourcePackRequest.resourcePackRequest().packs(info).required(force).prompt(prompt).build();
    }

    static final class Builder implements ResourcePackInfo.Builder {
        private final String url;
        private UUID id;
        private boolean force;
        private byte[] hash;
        private Component prompt;

        Builder(String url) {
            this.url = url;
        }

        @Override
        public Builder setId(UUID value) {
            id = value;
            return this;
        }

        @Override
        public Builder setShouldForce(boolean value) {
            force = value;
            return this;
        }

        @Override
        public Builder setHash(byte[] value) {
            if (value != null && value.length != 20) {
                throw new IllegalArgumentException("a resource pack hash is a SHA-1 of 20 bytes");
            }
            hash = value == null ? null : value.clone();
            return this;
        }

        @Override
        public Builder setPrompt(Component value) {
            prompt = value;
            return this;
        }

        @Override
        public ResourcePackInfo build() {
            return new VelocityPack(id, url, hash, force, prompt, Origin.PLUGIN_ON_PROXY);
        }
    }
}
