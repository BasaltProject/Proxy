// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import net.md_5.bungee.api.ChatColor;
import net.md_5.bungee.api.chat.BaseComponent;
import net.md_5.bungee.api.chat.TextComponent;

/** Writes components as the MiniMessage text the native proxy shows to players. */
final class MiniMessages {
    private MiniMessages() {}

    static String of(BaseComponent... components) {
        final StringBuilder out = new StringBuilder();
        if (components != null) {
            for (final BaseComponent component : components) {
                append(out, component);
            }
        }
        return out.toString();
    }

    private static void append(StringBuilder out, BaseComponent component) {
        final StringBuilder closing = new StringBuilder();
        final ChatColor color = component.getColorRaw();
        if (color != null) {
            out.append('<').append(color.getName()).append('>');
            closing.insert(0, "</" + (color.getName().startsWith("#") ? "color" : color.getName()) + ">");
        }
        tag(out, closing, component.isBoldRaw(), "bold");
        tag(out, closing, component.isItalicRaw(), "italic");
        tag(out, closing, component.isUnderlinedRaw(), "underlined");
        tag(out, closing, component.isStrikethroughRaw(), "strikethrough");
        tag(out, closing, component.isObfuscatedRaw(), "obfuscated");
        if (component instanceof TextComponent text) {
            out.append(escape(text.getText()));
            if (component.getExtra() != null) {
                for (final BaseComponent extra : component.getExtra()) {
                    append(out, extra);
                }
            }
        } else {
            out.append(escape(component.toPlainText()));
        }
        out.append(closing);
    }

    private static void tag(StringBuilder out, StringBuilder closing, Boolean value, String name) {
        if (value != null && value) {
            out.append('<').append(name).append('>');
            closing.insert(0, "</" + name + ">");
        }
    }

    private static String escape(String text) {
        return text.replace("\\", "\\\\").replace("<", "\\<");
    }
}
