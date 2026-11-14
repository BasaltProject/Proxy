// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.velocitypowered.api.plugin.PluginContainer;
import com.velocitypowered.api.plugin.PluginDescription;
import com.velocitypowered.api.plugin.PluginManager;
import com.velocitypowered.api.plugin.meta.PluginDependency;
import java.io.File;
import java.io.InputStreamReader;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.jar.JarFile;
import java.util.zip.ZipEntry;

/** Loads Velocity plugin jars in dependency order and builds their main class through Guice, as Velocity does. */
final class VelocityPlugins implements PluginManager {
    static final class Description implements PluginDescription {
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

    static final class Container implements PluginContainer {
        final Description description;
        final ExecutorService executor = Executors.newCachedThreadPool(runnable -> {
            final Thread thread = new Thread(runnable);
            thread.setDaemon(true);
            return thread;
        });
        volatile Object instance;
        volatile VelocityClassLoader loader;

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
        final Map<String, Candidate> found = new LinkedHashMap<>();
        for (final File jar : jars) {
            try {
                final Candidate candidate = describe(jar);
                if (candidate != null) {
                    found.putIfAbsent(candidate.description.id, candidate);
                }
            } catch (final Throwable error) {
                Natives.log("could not load " + jar.getName() + ": " + error);
            }
        }
        for (final Candidate candidate : ordered(found)) {
            try {
                loadJar(candidate);
            } catch (final Throwable error) {
                Failures.log("could not load " + candidate.jar.getName(), error);
            }
        }
    }

    private record Candidate(File jar, JsonObject json, Description description) {}

    private static Candidate describe(File jar) throws Exception {
        try (JarFile file = new JarFile(jar)) {
            final ZipEntry entry = file.getEntry("velocity-plugin.json");
            if (entry == null) {
                return null;
            }
            try (InputStreamReader reader = new InputStreamReader(file.getInputStream(entry))) {
                final JsonObject json = JsonParser.parseReader(reader).getAsJsonObject();
                return new Candidate(jar, json, new Description(json, jar.toPath()));
            }
        }
    }

    private static List<Candidate> ordered(Map<String, Candidate> found) {
        final List<Candidate> order = new ArrayList<>();
        final Set<String> visiting = new HashSet<>();
        final Set<String> done = new HashSet<>();
        for (final String id : found.keySet()) {
            visit(id, found, visiting, done, order);
        }
        return order;
    }

    private static boolean visit(String id, Map<String, Candidate> found, Set<String> visiting, Set<String> done,
            List<Candidate> order) {
        if (done.contains(id)) {
            return true;
        }
        final Candidate candidate = found.get(id);
        if (candidate == null || !visiting.add(id)) {
            return false;
        }
        for (final PluginDependency dependency : candidate.description.dependencies) {
            final boolean present = visit(dependency.getId(), found, visiting, done, order);
            if (!present && !dependency.isOptional()) {
                Natives.log("could not load " + id + ": it needs the plugin " + dependency.getId());
                visiting.remove(id);
                done.add(id);
                return false;
            }
        }
        visiting.remove(id);
        done.add(id);
        order.add(candidate);
        return true;
    }

    private void loadJar(Candidate candidate) throws Exception {
        final Description description = candidate.description;
        final Container container = new Container(description);
        final VelocityClassLoader loader = new VelocityClassLoader(candidate.jar.toURI().toURL(),
                BasaltVelocity.class.getClassLoader());
        container.loader = loader;
        final Class<?> main = loader.loadClass(candidate.json.get("main").getAsString());
        final Path data = candidate.jar.getParentFile().toPath().resolve(description.id);
        containers.put(description.id, container);
        try {
            container.instance = VelocityInjector.create(proxy, this, container, data).getInstance(main);
        } catch (final Throwable error) {
            containers.remove(description.id);
            throw error;
        }
        proxy.getEventManager().register(container.instance, container.instance);
        Natives.log("Loaded plugin " + description.id + " " + description.version);
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
        for (final Container container : containers.values()) {
            if (container.instance == plugin || plugin.getClass().getClassLoader() == container.loader) {
                try {
                    container.loader.add(path.toUri().toURL());
                } catch (java.net.MalformedURLException error) {
                    throw new IllegalArgumentException(error);
                }
                return;
            }
        }
        throw new IllegalArgumentException("plugin " + plugin + " is not loaded");
    }
}
