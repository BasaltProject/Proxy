// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import net.kyori.adventure.text.Component;
import net.kyori.adventure.text.minimessage.MiniMessage;
import net.kyori.adventure.text.serializer.gson.GsonComponentSerializer;

/** Components cross to the native side already serialized: JSON for chat, MiniMessage for decision answers. */
final class Text {
    private Text() {}

    static String json(Component component) {
        return GsonComponentSerializer.gson().serialize(component);
    }

    static String mini(Component component) {
        return component == null ? "" : MiniMessage.miniMessage().serialize(component);
    }

    static Component ofMini(String text) {
        return text == null || text.isEmpty() ? Component.empty() : MiniMessage.miniMessage().deserialize(text);
    }
}
