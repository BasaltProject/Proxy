// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import java.net.URL;
import java.net.URLClassLoader;
import java.util.Set;
import java.util.concurrent.CopyOnWriteArraySet;

/** One plugin's class loader; a class it lacks is looked up in the other plugins, as dependents expect. */
final class VelocityClassLoader extends URLClassLoader {
    private static final Set<VelocityClassLoader> kLoaders = new CopyOnWriteArraySet<>();

    static {
        ClassLoader.registerAsParallelCapable();
    }

    VelocityClassLoader(URL jar, ClassLoader parent) {
        super(new URL[] {jar}, parent);
        kLoaders.add(this);
    }

    synchronized void add(URL url) {
        addURL(url);
    }

    @Override
    protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
        try {
            return super.loadClass(name, resolve);
        } catch (final ClassNotFoundException missing) {
            for (final VelocityClassLoader other : kLoaders) {
                if (other == this) {
                    continue;
                }
                final Class<?> found = other.own(name);
                if (found != null) {
                    return found;
                }
            }
            throw missing;
        }
    }

    private Class<?> own(String name) {
        synchronized (getClassLoadingLock(name)) {
            final Class<?> loaded = findLoadedClass(name);
            if (loaded != null) {
                return loaded;
            }
            try {
                return findClass(name);
            } catch (final ClassNotFoundException | LinkageError absent) {
                return null;
            }
        }
    }
}
