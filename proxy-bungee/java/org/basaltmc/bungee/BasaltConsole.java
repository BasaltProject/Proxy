// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import java.util.Collection;
import java.util.List;
import net.md_5.bungee.api.CommandSender;
import net.md_5.bungee.api.chat.BaseComponent;
import net.md_5.bungee.api.chat.TextComponent;

final class BasaltConsole implements CommandSender {
    @Override
    public String getName() {
        return "CONSOLE";
    }

    @Override
    public void sendMessage(String message) {
        Natives.log(message);
    }

    @Override
    public void sendMessages(String... messages) {
        for (final String message : messages) {
            sendMessage(message);
        }
    }

    @Override
    public void sendMessage(BaseComponent... message) {
        Natives.log(TextComponent.toPlainText(message));
    }

    @Override
    public void sendMessage(BaseComponent message) {
        Natives.log(message.toPlainText());
    }

    @Override
    public Collection<String> getGroups() {
        return List.of();
    }

    @Override
    public void addGroups(String... groups) {}

    @Override
    public void removeGroups(String... groups) {}

    @Override
    public boolean hasPermission(String permission) {
        return true;
    }

    @Override
    public void setPermission(String permission, boolean value) {}

    @Override
    public Collection<String> getPermissions() {
        return List.of();
    }
}
