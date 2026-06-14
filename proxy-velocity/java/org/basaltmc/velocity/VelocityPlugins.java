// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.velocitypowered.api.plugin.PluginContainer;
import com.velocitypowered.api.plugin.PluginDescription;
import com.velocitypowered.api.plugin.PluginManager;
import com.velocitypowered.api.plugin.annotation.DataDirectory;
import com.velocitypowered.api.plugin.meta.PluginDependency;
import java.io.File;
import java.io.InputStreamReader;
import java.lang.annotation.Annotation;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.jar.JarFile;
import java.util.zip.ZipEntry;

/** Loads Velocity plugin jars and builds their main class with the few bindings plugins ask for. */
final class VelocityPlugins implements PluginManager {
    private static final class Description implements PluginDescription {
        final String id;
        final String name;
        final String version;
        final String description;
        final List<String> authors;
        final List<PluginDependency> dependencies;
        final Path source;

        Description(JsonObject json, Path source) {
            this.id = json.get("id").getAsString();
            this.name = json.has("name") ? json.get("name").getAsString() : null;
            this.version = json.has("version") ? json.get("version").getAsString() : null;
            this.description = json.has("description") ? json.get("description").getAsString() : null;
            this.authors = new ArrayList<>();
            if (json.has("authors")) {
                for (final JsonElement author : json.getAsJsonArray("authors")) {
                    authors.add(author.getAsString());
                }
            }
            this.dependencies = new ArrayList<>();
            if (json.has("dependencies")) {
                final JsonArray array = json.getAsJsonArray("dependencies");
                for (final JsonElement element : array) {
                    final JsonObject dependency = element.getAsJsonObject();
                    dependencies.add(new PluginDependency(dependency.get("id").getAsString(), null,
                            dependency.has("optional") && dependency.get("optional").getAsBoolean()));
                }
            }
            this.source = source;
        }

        @Override
        public String getId() {
            return id;
        }

        @Override
        public Optional<String> getName() {
            return Optional.ofNullable(name);
        }

        @Override
        public Optional<String> getVersion() {
            return Optional.ofNullable(version);
        }

        @Override
        public Optional<String> getDescription() {
            return Optional.ofNullable(description);
        }

        @Override
        public List<String> getAuthors() {
            return authors;
        }

        @Override
        public Collection<PluginDependency> getDependencies() {
            return dependencies;
        }

        @Override
        public Optional<Path> getSource() {
            return Optional.ofNullable(source);
        }
    }

    private static final class Container implements PluginContainer {
        final Description description;
        final ExecutorService executor = Executors.newCachedThreadPool(runnable -> {
            final Thread thread = new Thread(runnable);
            thread.setDaemon(true);
            return thread;
        });
        volatile Object instance;

        Container(Description description) {
            this.description = description;
        }

        @Override
        public PluginDescription getDescription() {
            return description;
        }

        @Override
        public Optional<?> getInstance() {
            return Optional.ofNullable(instance);
        }

        @Override
        public ExecutorService getExecutorService() {
            return executor;
        }
    }

    private final Map<String, Container> containers = new LinkedHashMap<>();
    private final BasaltProxy proxy;

    VelocityPlugins(BasaltProxy proxy) {
        this.proxy = proxy;
    }

    void load(File folder) {
        final File[] jars = folder.listFiles((dir, name) -> name.endsWith(".jar"));
        if (jars == null) {
            return;
        }
        Arrays.sort(jars);
        for (final File jar : jars) {
            try {
                loadJar(jar);
            } catch (final Throwable error) {
                Natives.log("could not load " + jar.getName() + ": " + error);
            }
        }
    }

    private void loadJar(File jar) throws Exception {
        final JsonObject json;
        try (JarFile file = new JarFile(jar)) {
            final ZipEntry entry = file.getEntry("velocity-plugin.json");
            if (entry == null) {
                return;
            }
            try (InputStreamReader reader = new InputStreamReader(file.getInputStream(entry))) {
                json = JsonParser.parseReader(reader).getAsJsonObject();
            }
        }
        final Description description = new Description(json, jar.toPath());
        final Container container = new Container(description);
        final URLClassLoader loader = new URLClassLoader(new URL[] {jar.toURI().toURL()}, BasaltVelocity.class.getClassLoader());
        final Class<?> main = loader.loadClass(json.get("main").getAsString());
        final Path data = jar.getParentFile().toPath().resolve(description.id);
        container.instance = construct(main, container, data);
        containers.put(description.id, container);
        proxy.getEventManager().register(container.instance, container.instance);
        Natives.log("Loaded plugin " + description.id + " " + description.version);
    }

    private Object resolve(Class<?> type, Annotation[] annotations, Container container, Path data) {
        for (final Annotation annotation : annotations) {
            if (annotation instanceof DataDirectory) {
                return data;
            }
        }
        if (type.isInstance(proxy)) {
            return proxy;
        }
        if (type == org.slf4j.Logger.class) {
            return new VelocityLogger(container.description.id);
        }
        if (type == PluginContainer.class) {
            return container;
        }
        if (type.isInstance(proxy.getEventManager())) {
            return proxy.getEventManager();
        }
        if (type.isInstance(proxy.getCommandManager())) {
            return proxy.getCommandManager();
        }
        if (type.isInstance(this)) {
            return this;
        }
        throw new IllegalStateException("no binding for " + type.getName());
    }

    private static boolean injects(Annotation[] annotations) {
        for (final Annotation annotation : annotations) {
            if (annotation.annotationType().getSimpleName().equals("Inject")) {
                return true;
            }
        }
        return false;
    }

    private Object construct(Class<?> main, Container container, Path data) throws Exception {
        Constructor<?> chosen = null;
        for (final Constructor<?> candidate : main.getDeclaredConstructors()) {
            if (injects(candidate.getAnnotations())) {
                chosen = candidate;
            }
        }
        if (chosen == null) {
            chosen = main.getDeclaredConstructor();
        }
        chosen.setAccessible(true);
        final Class<?>[] types = chosen.getParameterTypes();
        final Annotation[][] annotations = chosen.getParameterAnnotations();
        final Object[] arguments = new Object[types.length];
        for (int i = 0; i < types.length; i++) {
            arguments[i] = resolve(types[i], annotations[i], container, data);
        }
        final Object instance = chosen.newInstance(arguments);
        for (final Field field : main.getDeclaredFields()) {
            if (injects(field.getAnnotations())) {
                field.setAccessible(true);
                field.set(instance, resolve(field.getType(), field.getAnnotations(), container, data));
            }
        }
        return instance;
    }

    String[] summary() {
        final List<String> rows = new ArrayList<>();
        for (final Container container : containers.values()) {
            rows.add(container.description.getName().orElse(container.description.id) + "\t"
                    + container.description.getVersion().orElse(""));
        }
        return rows.toArray(new String[0]);
    }

    @Override
    public Optional<PluginContainer> fromInstance(Object instance) {
        for (final Container container : containers.values()) {
            if (container.instance == instance) {
                return Optional.of(container);
            }
        }
        return Optional.empty();
    }

    @Override
    public Optional<PluginContainer> getPlugin(String id) {
        return Optional.ofNullable(containers.get(id));
    }

    @Override
    public Collection<PluginContainer> getPlugins() {
        return new ArrayList<>(containers.values());
    }

    @Override
    public boolean isLoaded(String id) {
        return containers.containsKey(id);
    }

    @Override
    public void addToClasspath(Object plugin, Path path) {
        throw new UnsupportedOperationException("adding to a plugin classpath is not available yet");
    }
}
