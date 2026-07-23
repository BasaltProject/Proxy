// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import net.md_5.bungee.api.Title;
import net.md_5.bungee.api.chat.BaseComponent;
import net.md_5.bungee.api.connection.ProxiedPlayer;

final class BasaltTitle implements Title {
    private BaseComponent[] title;
    private BaseComponent[] subTitle;
    private int fadeIn = -1;
    private int stay = -1;
    private int fadeOut = -1;
    private boolean clear;
    private boolean reset;

    @Override
    public Title title(BaseComponent text) {
        return title(new BaseComponent[] {text});
    }

    @Override
    public Title title(BaseComponent... text) {
        this.title = text;
        return this;
    }

    @Override
    public Title subTitle(BaseComponent text) {
        return subTitle(new BaseComponent[] {text});
    }

    @Override
    public Title subTitle(BaseComponent... text) {
        this.subTitle = text;
        return this;
    }

    @Override
    public Title fadeIn(int ticks) {
        this.fadeIn = ticks;
        return this;
    }

    @Override
    public Title stay(int ticks) {
        this.stay = ticks;
        return this;
    }

    @Override
    public Title fadeOut(int ticks) {
        this.fadeOut = ticks;
        return this;
    }

    @Override
    public Title clear() {
        this.clear = true;
        return this;
    }

    @Override
    public Title reset() {
        this.reset = true;
        return this;
    }

    @Override
    public Title send(ProxiedPlayer player) {
        String name = player.getName();
        if (clear || reset) {
            Hud.send(name, Hud.CLEAR_TITLE, null, null, reset ? 1 : 0, 0, 0, 0, null);
            clear = false;
            reset = false;
        }
        if (title != null || subTitle != null || fadeIn >= 0 || stay >= 0 || fadeOut >= 0) {
            boolean timed = fadeIn >= 0 || stay >= 0 || fadeOut >= 0;
            Hud.send(name, Hud.TITLE, Hud.json(title), Hud.json(subTitle), timed ? (fadeIn < 0 ? 10 : fadeIn) : -1,
                    timed ? (stay < 0 ? 70 : stay) : -1, timed ? (fadeOut < 0 ? 20 : fadeOut) : -1, 0, null);
        }
        return this;
    }
}
