// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.velocitypowered.api.proxy.messages.ChannelIdentifier;
import com.velocitypowered.api.proxy.messages.ChannelRegistrar;

final class VelocityChannels implements ChannelRegistrar {
    @Override
    public void register(ChannelIdentifier... identifiers) {
        for (final ChannelIdentifier identifier : identifiers) {
            Natives.channel(identifier.getId(), true);
            BasaltVelocity.channels().add(identifier.getId());
        }
    }

    @Override
    public void unregister(ChannelIdentifier... identifiers) {
        for (final ChannelIdentifier identifier : identifiers) {
            Natives.channel(identifier.getId(), false);
            BasaltVelocity.channels().remove(identifier.getId());
        }
    }
}
