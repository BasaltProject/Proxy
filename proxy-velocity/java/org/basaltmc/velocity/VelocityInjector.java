// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.google.inject.AbstractModule;
import com.google.inject.Guice;
import com.google.inject.Injector;
import com.google.inject.Stage;
import com.velocitypowered.api.command.CommandManager;
import com.velocitypowered.api.event.EventManager;
import com.velocitypowered.api.plugin.PluginContainer;
import com.velocitypowered.api.plugin.PluginDescription;
import com.velocitypowered.api.plugin.PluginManager;
import com.velocitypowered.api.plugin.annotation.DataDirectory;
import com.velocitypowered.api.proxy.ProxyServer;
import java.nio.file.Path;
import java.util.concurrent.ExecutorService;
import net.kyori.adventure.text.logger.slf4j.ComponentLogger;
import org.slf4j.Logger;

/** The Guice bindings a Velocity plugin may inject: the proxy services and its own logger, folder and container. */
final class VelocityInjector {
    static {
        if (System.getProperty("guice_bytecode_gen_option") == null) {
            System.setProperty("guice_bytecode_gen_option", "DISABLED");
        }
    }

    private VelocityInjector() {}

    static Injector create(BasaltProxy proxy, PluginManager plugins, VelocityPlugins.Container container, Path data) {
        final String id = container.description.getId();
        return Guice.createInjector(Stage.PRODUCTION, new AbstractModule() {
            @Override
            protected void configure() {
                bind(ProxyServer.class).toInstance(proxy);
                bind(PluginManager.class).toInstance(plugins);
                bind(EventManager.class).toInstance(proxy.getEventManager());
                bind(CommandManager.class).toInstance(proxy.getCommandManager());
                bind(Logger.class).toInstance(new VelocityLogger(id));
                bind(ComponentLogger.class).toInstance(ComponentLogger.logger(id));
                bind(Path.class).annotatedWith(DataDirectory.class).toInstance(data);
                bind(PluginDescription.class).toInstance(container.description);
                bind(PluginContainer.class).toInstance(container);
                bind(ExecutorService.class).toInstance(container.getExecutorService());
            }
        });
    }
}
