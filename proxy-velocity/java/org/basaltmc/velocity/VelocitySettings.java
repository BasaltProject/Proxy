// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.velocitypowered.api.proxy.player.PlayerSettings;
import com.velocitypowered.api.proxy.player.SkinParts;
import java.util.Locale;

/** The client information the proxy last saw, in the field order of {@code Natives.settings}. */
final class VelocitySettings implements PlayerSettings {
    static final VelocitySettings DEFAULT = new VelocitySettings(new String[] {"en_us", "10", "0", "1", "127", "1", "0", "1", "0"}, 0);

    private final String[] f;
    private final int at;

    VelocitySettings(String[] fields, int offset) {
        this.f = fields;
        this.at = offset;
    }

    static VelocitySettings of(String player) {
        final String[] fields = Natives.settings(player);
        return fields == null ? null : new VelocitySettings(fields, 0);
    }

    private int number(int index) {
        return Integer.parseInt(f[at + index]);
    }

    @Override
    public Locale getLocale() {
        return Locale.forLanguageTag(f[at].replace('_', '-'));
    }

    @Override
    public byte getViewDistance() {
        return (byte) number(1);
    }

    @Override
    public ChatMode getChatMode() {
        final ChatMode[] modes = ChatMode.values();
        return modes[Math.max(0, Math.min(modes.length - 1, number(2)))];
    }

    @Override
    public boolean hasChatColors() {
        return number(3) != 0;
    }

    @Override
    public SkinParts getSkinParts() {
        return new SkinParts((byte) number(4));
    }

    @Override
    public MainHand getMainHand() {
        return number(5) == 0 ? MainHand.LEFT : MainHand.RIGHT;
    }

    @Override
    public boolean isClientListingAllowed() {
        return number(7) != 0;
    }

    @Override
    public boolean isTextFilteringEnabled() {
        return number(6) != 0;
    }

    @Override
    public ParticleStatus getParticleStatus() {
        final ParticleStatus[] values = ParticleStatus.values();
        return values[Math.max(0, Math.min(values.length - 1, number(8)))];
    }
}
