package org.basaltmc.velocitytest;

import com.google.inject.Inject;
import com.velocitypowered.api.command.CommandSource;
import com.velocitypowered.api.command.SimpleCommand;
import com.velocitypowered.api.event.EventTask;
import com.velocitypowered.api.event.Subscribe;
import com.velocitypowered.api.event.connection.PostLoginEvent;
import com.velocitypowered.api.event.connection.PreLoginEvent;
import com.velocitypowered.api.event.player.PlayerChatEvent;
import com.velocitypowered.api.event.connection.PluginMessageEvent;
import com.velocitypowered.api.event.player.KickedFromServerEvent;
import com.velocitypowered.api.event.player.PlayerChooseInitialServerEvent;
import com.velocitypowered.api.event.player.ServerPreConnectEvent;
import com.velocitypowered.api.proxy.Player;
import com.velocitypowered.api.proxy.messages.LegacyChannelIdentifier;
import com.velocitypowered.api.proxy.messages.MinecraftChannelIdentifier;
import com.velocitypowered.api.event.proxy.ProxyInitializeEvent;
import com.velocitypowered.api.event.proxy.ProxyPingEvent;
import com.velocitypowered.api.plugin.Plugin;
import com.velocitypowered.api.plugin.annotation.DataDirectory;
import com.velocitypowered.api.proxy.ProxyServer;
import java.nio.file.Path;
import net.kyori.adventure.text.Component;
import org.slf4j.Logger;

@Plugin(id = "basalt-velocity-test", name = "BasaltVelocityTest", version = "1.0", authors = {"Basalt"})
public final class TestPlugin {
    private final ProxyServer server;
    private final Logger logger;
    private final Path data;
    private volatile boolean denyNext;
    private volatile boolean redirectNext;

    @Inject
    public TestPlugin(ProxyServer server, Logger logger, @DataDirectory Path data) {
        this.server = server;
        this.logger = logger;
        this.data = data;
    }

    @Subscribe
    public void onJoin(PostLoginEvent event) {
        final Player player = event.getPlayer();
        server.getScheduler().buildTask(this, () -> {
            player.showTitle(net.kyori.adventure.title.Title.title(Component.text("Velocity title"), Component.text("sub")));
            player.sendActionBar(Component.text("velocity action bar"));
            player.sendPlayerListHeaderAndFooter(Component.text("vel header"), Component.text("vel footer"));
            final net.kyori.adventure.bossbar.BossBar bar = net.kyori.adventure.bossbar.BossBar.bossBar(Component.text("vel boss"), 0.5f,
                    net.kyori.adventure.bossbar.BossBar.Color.GREEN, net.kyori.adventure.bossbar.BossBar.Overlay.PROGRESS);
            player.showBossBar(bar);
            player.getTabList().addEntry(player.getTabList().buildEntry(
                    new com.velocitypowered.api.util.GameProfile(java.util.UUID.nameUUIDFromBytes("row".getBytes()), "Row1", java.util.List.of()),
                    Component.text("proxy row"), 42, 0));
            bar.progress(0.2f);
            player.storeCookie(net.kyori.adventure.key.Key.key("basalt", "velocity"), new byte[] {9, 8});
            logger.info("hud sent; permission " + player.hasPermission("basalt.velocity"));
        }).delay(2, java.util.concurrent.TimeUnit.SECONDS).schedule();
        server.getScheduler().buildTask(this, () -> player.requestCookie(net.kyori.adventure.key.Key.key("basalt", "velocity")))
                .delay(4, java.util.concurrent.TimeUnit.SECONDS).schedule();
    }

    @Subscribe
    public void onCookie(com.velocitypowered.api.event.player.CookieReceiveEvent event) {
        logger.info("cookie " + event.getOriginalKey() + " " + (event.getOriginalData() == null ? "null" : event.getOriginalData().length));
    }

    @Subscribe
    public void onInit(ProxyInitializeEvent event) {
        server.getCommandManager().register(server.getCommandManager().metaBuilder("veltest").plugin(this).build(),
                (SimpleCommand) invocation -> {
                    final String[] args = invocation.arguments();
                    if (args.length == 1 && args[0].equals("redir")) {
                        redirectNext = true;
                    }
                    if (args.length == 1 && args[0].equals("deny")) {
                        denyNext = true;
                    }
                    if (args.length == 2 && args[0].equals("go") && invocation.source() instanceof Player player) {
                        server.getServer(args[1]).ifPresent(target -> player.createConnectionRequest(target).fireAndForget());
                    }
                    invocation.source().sendMessage(Component.text("veltest ok " + String.join(",", args)));
                });
        server.getChannelRegistrar().register(new LegacyChannelIdentifier("BungeeCord"), MinecraftChannelIdentifier.create("basalt", "pm"));
        logger.info("BasaltVelocityTest enabled, data directory {}", data.getFileName());
    }

    @Subscribe
    public EventTask onPreLogin(PreLoginEvent event) {
        return EventTask.async(() -> {
            logger.info("prelogin {}", event.getUsername());
            if (event.getUsername().startsWith("Blocked")) {
                event.setResult(PreLoginEvent.PreLoginComponentResult.denied(Component.text("Blocked by BasaltVelocityTest")));
            }
        });
    }

    @Subscribe
    public void onPostLogin(PostLoginEvent event) {
        logger.info("postlogin {}", event.getPlayer().getUsername());
    }

    @Subscribe
    public void onChat(PlayerChatEvent event) {
        logger.info("chat {}", event.getMessage());
        if (event.getMessage().contains("forbidden")) {
            event.setResult(PlayerChatEvent.ChatResult.denied());
        }
    }

    @Subscribe
    public void onPing(ProxyPingEvent event) {
        event.setPing(event.getPing().asBuilder().description(Component.text("Basalt via Velocity API")).build());
    }

    @Subscribe
    public void onChoose(PlayerChooseInitialServerEvent event) {
        logger.info("choose initial {}", event.getInitialServer().map(s -> s.getServerInfo().getName()).orElse("none"));
    }

    @Subscribe
    public void onPreConnect(ServerPreConnectEvent event) {
        logger.info("preconnect {}", event.getOriginalServer().getServerInfo().getName());
        if (redirectNext && event.getOriginalServer().getServerInfo().getName().equals("b")) {
            redirectNext = false;
            server.getServer("lobby").ifPresent(lobby -> event.setResult(ServerPreConnectEvent.ServerResult.allowed(lobby)));
            return;
        }
        if (denyNext && event.getOriginalServer().getServerInfo().getName().equals("b")) {
            denyNext = false;
            event.setResult(ServerPreConnectEvent.ServerResult.denied());
        }
    }

    @Subscribe
    public void onKicked(KickedFromServerEvent event) {
        logger.info("kicked from {}", event.getServer().getServerInfo().getName());
        server.getServer("lobby").ifPresent(lobby -> {
            if (!event.getServer().getServerInfo().getName().equals("lobby")) {
                event.setResult(KickedFromServerEvent.RedirectPlayer.create(lobby));
            }
        });
    }

    @Subscribe
    public void onMessage(PluginMessageEvent event) {
        logger.info("message {} {}", event.getIdentifier().getId(), event.getData().length);
        if (event.getIdentifier().getId().equals("basalt:pm") && event.getSource() instanceof com.velocitypowered.api.proxy.ServerConnection backend) {
            final com.google.common.io.ByteArrayDataInput in = com.google.common.io.ByteStreams.newDataInput(event.getData());
            final String text = in.readUTF();
            logger.info("pm from backend {}", text);
            event.setResult(PluginMessageEvent.ForwardResult.handled());
            if (!text.startsWith("echo")) {
                backend.sendPluginMessage(MinecraftChannelIdentifier.create("basalt", "pm"), out -> out.writeUTF("ping from velocity"));
            }
        }
    }
}
