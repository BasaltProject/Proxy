package org.basaltmc.bungeetest;

import net.md_5.bungee.api.CommandSender;
import net.md_5.bungee.api.chat.TextComponent;
import net.md_5.bungee.api.event.ChatEvent;
import net.md_5.bungee.api.event.PostLoginEvent;
import net.md_5.bungee.api.event.PreLoginEvent;
import net.md_5.bungee.api.event.PluginMessageEvent;
import net.md_5.bungee.api.event.ProxyPingEvent;
import net.md_5.bungee.api.event.ServerKickEvent;
import net.md_5.bungee.api.event.ServerConnectEvent;
import net.md_5.bungee.api.connection.ProxiedPlayer;
import net.md_5.bungee.api.plugin.Command;
import net.md_5.bungee.api.plugin.Listener;
import net.md_5.bungee.api.plugin.Plugin;
import net.md_5.bungee.event.EventHandler;
import net.md_5.bungee.event.EventPriority;

public final class TestPlugin extends Plugin implements Listener {
    @Override
    public void onEnable() {
        getProxy().getPluginManager().registerListener(this, this);
        getProxy().getPluginManager().registerCommand(this, new Command("bungeetest") {
            @Override
            public void execute(CommandSender sender, String[] args) {
                if (args.length == 2 && args[0].equals("go") && sender instanceof ProxiedPlayer player) {
                    player.connect(getProxy().getServerInfo(args[1]));
                }
                sender.sendMessage(new TextComponent("bungeetest ok " + String.join(",", args)));
            }
        });
        getLogger().info("BasaltBungeeTest enabled");
    }

    @EventHandler(priority = EventPriority.HIGH)
    public void onPreLogin(PreLoginEvent event) {
        getLogger().info("prelogin " + event.getConnection().getName());
        if (event.getConnection().getName().startsWith("Blocked")) {
            event.setCancelled(true);
            event.setReason(new TextComponent("Blocked by BasaltBungeeTest"));
        }
    }

    @EventHandler
    public void onPostLogin(PostLoginEvent event) {
        getLogger().info("postlogin " + event.getPlayer().getName());
        final ProxiedPlayer player = event.getPlayer();
        getProxy().getScheduler().schedule(this, () -> {
            player.sendTitle(getProxy().createTitle().title(new TextComponent("Bungee title")).subTitle(new TextComponent("sub"))
                    .fadeIn(5).stay(40).fadeOut(10));
            player.sendMessage(net.md_5.bungee.api.ChatMessageType.ACTION_BAR, new TextComponent("bungee action bar"));
            player.setTabHeader(new TextComponent("bungee header"), new TextComponent("bungee footer"));
            player.storeCookie("basalt:bungee", new byte[] {1, 2, 3});
            getLogger().info("hud sent; permission " + player.hasPermission("basalt.bungee"));
        }, 2, java.util.concurrent.TimeUnit.SECONDS);
        getProxy().getScheduler().schedule(this, () -> player.retrieveCookie("basalt:bungee").whenComplete(
                (data, error) -> getLogger().info("cookie " + (data == null ? "null" : data.length) + " " + error)), 4,
                java.util.concurrent.TimeUnit.SECONDS);
    }

    @EventHandler
    public void onChat(ChatEvent event) {
        getLogger().info("chat " + event.getMessage());
        if (event.getMessage().contains("forbidden")) {
            event.setCancelled(true);
        }
    }

    @EventHandler
    public void onPing(ProxyPingEvent event) {
        event.getResponse().setDescriptionComponent(new TextComponent("Basalt via BungeeCord API"));
    }

    @EventHandler
    public void onKick(ServerKickEvent event) {
        getLogger().info("kick " + event.getPlayer().getName() + " from " + event.getKickedFrom().getName());
        if (event.getKickedFrom().getName().equals("b") && getProxy().getServerInfo("lobby") != null) {
            event.setCancelled(true);
            event.setCancelServer(getProxy().getServerInfo("lobby"));
        }
    }

    @EventHandler
    public void onConnect(ServerConnectEvent event) {
        getLogger().info("connect " + event.getPlayer().getName() + " -> " + event.getTarget().getName());
    }

    @EventHandler
    public void onMessage(PluginMessageEvent event) {
        getLogger().info("message " + event.getTag() + " " + event.getData().length);
    }
}
