// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import java.io.IOException;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;
import net.md_5.bungee.api.plugin.Plugin;

/** The context loader of plugin threads: it also reaches every plugin's classes and resources. */
final class PluginContextLoader extends ClassLoader {
    PluginContextLoader() {
        super(PluginContextLoader.class.getClassLoader());
    }

    private static List<ClassLoader> plugins() {
        final List<ClassLoader> loaders = new ArrayList<>();
        final BasaltProxy proxy = BasaltBungee.proxy();
        if (proxy != null) {
            for (final Plugin plugin : proxy.getPluginManager().getPlugins()) {
                loaders.add(plugin.getClass().getClassLoader());
            }
        }
        return loaders;
    }

    @Override
    protected Class<?> findClass(String name) throws ClassNotFoundException {
        for (final ClassLoader loader : plugins()) {
            try {
                return loader.loadClass(name);
            } catch (final ClassNotFoundException absent) {
                continue;
            }
        }
        throw new ClassNotFoundException(name);
    }

    @Override
    protected URL findResource(String name) {
        for (final ClassLoader loader : plugins()) {
            final URL found = loader.getResource(name);
            if (found != null) {
                return found;
            }
        }
        return null;
    }

    @Override
    protected Enumeration<URL> findResources(String name) throws IOException {
        final List<URL> found = new ArrayList<>();
        for (final ClassLoader loader : plugins()) {
            found.addAll(Collections.list(loader.getResources(name)));
        }
        return Collections.enumeration(found);
    }
}
