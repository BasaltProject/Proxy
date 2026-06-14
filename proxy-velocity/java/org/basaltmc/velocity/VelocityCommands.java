// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import com.mojang.brigadier.CommandDispatcher;
import com.mojang.brigadier.ParseResults;
import com.mojang.brigadier.suggestion.Suggestions;
import com.velocitypowered.api.command.BrigadierCommand;
import com.velocitypowered.api.command.Command;
import com.velocitypowered.api.command.CommandManager;
import com.velocitypowered.api.command.CommandMeta;
import com.velocitypowered.api.command.CommandSource;
import com.velocitypowered.api.command.RawCommand;
import com.velocitypowered.api.command.SimpleCommand;
import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CompletableFuture;

final class VelocityCommands implements CommandManager {
    private final Map<String, Command> commands = new LinkedHashMap<>();
    private final Map<String, CommandMeta> metas = new LinkedHashMap<>();
    private final CommandDispatcher<CommandSource> dispatcher = new CommandDispatcher<>();

    private static final class Meta implements CommandMeta {
        private final List<String> aliases;
        private final Object plugin;

        Meta(List<String> aliases, Object plugin) {
            this.aliases = aliases;
            this.plugin = plugin;
        }

        @Override
        public Collection<String> getAliases() {
            return aliases;
        }

        @Override
        public Collection<com.mojang.brigadier.tree.CommandNode<CommandSource>> getHints() {
            return List.of();
        }

        @Override
        public Object getPlugin() {
            return plugin;
        }
    }

    private static final class MetaBuilder implements CommandMeta.Builder {
        private final List<String> aliases = new ArrayList<>();
        private Object plugin;

        MetaBuilder(String first) {
            aliases.add(first);
        }

        @Override
        public CommandMeta.Builder aliases(String... more) {
            aliases.addAll(List.of(more));
            return this;
        }

        @Override
        public CommandMeta.Builder hint(com.mojang.brigadier.tree.CommandNode<CommandSource> node) {
            return this;
        }

        @Override
        public CommandMeta.Builder plugin(Object owner) {
            plugin = owner;
            return this;
        }

        @Override
        public CommandMeta build() {
            return new Meta(aliases, plugin);
        }
    }

    @Override
    public CommandMeta.Builder metaBuilder(String alias) {
        return new MetaBuilder(alias.toLowerCase(Locale.ROOT));
    }

    @Override
    public CommandMeta.Builder metaBuilder(BrigadierCommand command) {
        return new MetaBuilder(command.getNode().getLiteral().toLowerCase(Locale.ROOT));
    }

    @Override
    public void register(BrigadierCommand command) {
        dispatcher.getRoot().addChild(command.getNode());
        commands.put(command.getNode().getLiteral().toLowerCase(Locale.ROOT), command);
    }

    @Override
    public void register(CommandMeta meta, Command command) {
        for (final String alias : meta.getAliases()) {
            commands.put(alias.toLowerCase(Locale.ROOT), command);
            metas.put(alias.toLowerCase(Locale.ROOT), meta);
        }
    }

    @Override
    public void unregister(String alias) {
        commands.remove(alias.toLowerCase(Locale.ROOT));
        metas.remove(alias.toLowerCase(Locale.ROOT));
    }

    @Override
    public void unregister(CommandMeta meta) {
        for (final String alias : meta.getAliases()) {
            unregister(alias);
        }
    }

    @Override
    public CommandMeta getCommandMeta(String alias) {
        return metas.get(alias.toLowerCase(Locale.ROOT));
    }

    @Override
    public CompletableFuture<Boolean> executeAsync(CommandSource source, String line) {
        return executeImmediatelyAsync(source, line);
    }

    @Override
    public CompletableFuture<Boolean> executeImmediatelyAsync(CommandSource source, String line) {
        final String trimmed = line.strip();
        final int space = trimmed.indexOf(' ');
        final String root = (space < 0 ? trimmed : trimmed.substring(0, space)).toLowerCase(Locale.ROOT);
        final String rest = space < 0 ? "" : trimmed.substring(space + 1);
        final Command command = commands.get(root);
        if (command == null) {
            return CompletableFuture.completedFuture(false);
        }
        try {
            if (command instanceof BrigadierCommand) {
                final ParseResults<CommandSource> parsed = dispatcher.parse(trimmed, source);
                dispatcher.execute(parsed);
            } else if (command instanceof SimpleCommand simple) {
                final String[] arguments = rest.isEmpty() ? new String[0] : rest.split(" ");
                final SimpleCommand.Invocation invocation = new SimpleCommand.Invocation() {
                    @Override
                    public String alias() {
                        return root;
                    }

                    @Override
                    public CommandSource source() {
                        return source;
                    }

                    @Override
                    public String[] arguments() {
                        return arguments;
                    }
                };
                if (simple.hasPermission(invocation)) {
                    simple.execute(invocation);
                } else {
                    return CompletableFuture.completedFuture(true);
                }
            } else if (command instanceof RawCommand raw) {
                final RawCommand.Invocation invocation = new RawCommand.Invocation() {
                    @Override
                    public String alias() {
                        return root;
                    }

                    @Override
                    public CommandSource source() {
                        return source;
                    }

                    @Override
                    public String arguments() {
                        return rest;
                    }
                };
                if (raw.hasPermission(invocation)) {
                    raw.execute(invocation);
                }
            }
        } catch (final Throwable error) {
            Natives.log("the command " + root + " failed: " + error);
        }
        return CompletableFuture.completedFuture(true);
    }

    @Override
    public CompletableFuture<List<String>> offerSuggestions(CommandSource source, String line) {
        return CompletableFuture.completedFuture(List.of());
    }

    @Override
    public CompletableFuture<Suggestions> offerBrigadierSuggestions(CommandSource source, String line) {
        return dispatcher.getCompletionSuggestions(dispatcher.parse(line, source));
    }

    @Override
    public Collection<String> getAliases() {
        return commands.keySet();
    }

    @Override
    public boolean hasCommand(String alias) {
        return commands.containsKey(alias.toLowerCase(Locale.ROOT));
    }

    @Override
    public boolean hasCommand(String alias, CommandSource source) {
        return hasCommand(alias);
    }
}
