// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import net.md_5.bungee.api.Title;
import net.md_5.bungee.api.chat.BaseComponent;
import net.md_5.bungee.api.connection.ProxiedPlayer;

final class BasaltTitle implements Title {
    private Title unsupported() {
        throw new UnsupportedOperationException("titles are not available yet");
    }

    @Override
    public Title title(BaseComponent text) {
        return unsupported();
    }

    @Override
    public Title title(BaseComponent... text) {
        return unsupported();
    }

    @Override
    public Title subTitle(BaseComponent text) {
        return unsupported();
    }

    @Override
    public Title subTitle(BaseComponent... text) {
        return unsupported();
    }

    @Override
    public Title fadeIn(int ticks) {
        return unsupported();
    }

    @Override
    public Title stay(int ticks) {
        return unsupported();
    }

    @Override
    public Title fadeOut(int ticks) {
        return unsupported();
    }

    @Override
    public Title clear() {
        return unsupported();
    }

    @Override
    public Title reset() {
        return unsupported();
    }

    @Override
    public Title send(ProxiedPlayer player) {
        return unsupported();
    }
}
