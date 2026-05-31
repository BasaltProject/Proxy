#include "BuiltinCommands.hpp"

#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/common/Uuid.hpp>
#include <basalt/wire/common/placeholder/Placeholders.hpp>

#include <algorithm>
#include <map>

namespace basalt::proxy::runtime::detail {
namespace {

using Node = CommandNode;
using Kind = ArgumentKind;

constexpr std::string_view kOwner = "basalt";

} // namespace

void ensureKnown(const CommandEnvironment& environment, const PlayerIdentity& player) {
    const bool known = environment.players->locked([&](auto& players, auto&) { return players.exists(player.uuid); });
    if (!known)
        environment.players->recordLogin(player.uuid, player.name, std::nullopt);
}

namespace {

void add(const CommandEnvironment& environment, std::string name, std::string description, Node root,
         std::vector<std::string> aliases = {}) {
    const std::string permission = "basalt.command." + name;
    CommandRegistration registration;
    registration.root = std::move(root);
    aliases.push_back("basalt:" + name);
    registration.aliases = std::move(aliases);
    registration.permission = permission;
    registration.description = std::move(description);
    registration.owner = std::string{kOwner};
    static_cast<void>(environment.dispatcher->add(std::move(registration)));
}

} // namespace

std::string_view sendMessageKey(SendResult result, std::string_view accepted) {
    switch (result) {
    case SendResult::kAccepted:
        return accepted;
    case SendResult::kNotFound:
        return "send-not-found";
    case SendResult::kUnavailable:
        return "send-unavailable";
    case SendResult::kBusy:
        return "send-busy";
    case SendResult::kStopped:
        return "send-stopped";
    }
    return "send-unavailable";
}

bool hasServer(const CommandEnvironment& environment, std::string_view name) {
    const auto names = environment.servers();
    return std::ranges::find(names, name) != names.end();
}

std::string joined(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names)
        text += (text.empty() ? "" : ", ") + name;
    return text.empty() ? "-" : text;
}

std::optional<PlayerIdentity> resolvePlayer(const CommandEnvironment& environment, std::string_view name) {
    if (const auto online = environment.roster->find(name))
        return PlayerIdentity{online->name, online->uuid};
    auto stored = environment.players->locked([&](auto& players, auto&) -> std::optional<PlayerIdentity> {
        const auto uuid = players.findUuidByName(name);
        return uuid ? std::optional{PlayerIdentity{std::string{name}, *uuid}} : std::nullopt;
    });
    if (stored)
        return stored;
    if (!environment.onlineMode && wire::auth::validLoginName(name))
        return PlayerIdentity{std::string{name}, wire::common::Uuid::offlinePlayer(name).toString()};
    return std::nullopt;
}

void registerBuiltinCommands(const CommandEnvironment& environment) {
    const CommandEnvironment env = environment;
    const auto say = [env](const CommandContext& context, std::string_view key, const MessageArgs& args = {}) {
        context.reply(env.messages->command(key, args));
    };

    add(env, "help", "Lists the commands you can use.",
        Node::literal("help").executes([env](const CommandContext& context) {
            context.reply(env.messages->command("help-header"));
            for (const HelpEntry& entry : env.dispatcher->help(context.source()))
                context.reply(env.messages->command("help-entry", {{"usage", entry.usage},
                                                                   {"description", entry.description}}));
        }),
        {"?"});

    add(env, "list", "Shows how many players are online.",
        Node::literal("list").executes([env](const CommandContext& context) {
            std::vector<std::string> names;
            for (const auto& entry : env.roster->all())
                names.push_back(entry.name);
            context.reply(env.messages->command("list", {{"online", std::to_string(names.size())},
                                                         {"players", joined(names)}}));
        }));

    add(env, "glist", "Shows the players on every server.",
        Node::literal("glist").executes([env](const CommandContext& context) {
            std::map<std::string, std::vector<std::string>> byServer;
            for (const std::string& server : env.servers())
                byServer[server];
            const auto entries = env.roster->all();
            for (const auto& entry : entries)
                byServer[entry.backend].push_back(entry.name);
            for (const auto& [server, names] : byServer)
                context.reply(env.messages->command("glist-server", {{"server", server},
                                                                     {"count", std::to_string(names.size())},
                                                                     {"players", joined(names)}}));
            context.reply(env.messages->command("glist-total", {{"total", std::to_string(entries.size())}}));
        }));

    Node server = Node::literal("server");
    server.executes([env](const CommandContext& context) {
        const auto current = env.roster->find(context.source().name);
        context.reply(env.messages->command(
            "server-current", {{"server", current ? current->backend : std::string{"-"}},
                               {"servers", joined(env.servers())}}));
    });
    server.then(Node::argument("server", Kind::Server).executes([env](const CommandContext& context) {
        const std::string target{context.text("server")};
        if (context.source().console) {
            context.reply(env.messages->command("server-players-only"));
        } else if (!hasServer(env, target)) {
            context.reply(env.messages->command("unknown-server", {{"server", target}}));
        } else {
            context.reply(env.messages->command(sendMessageKey(env.send(context.source().name, target), "server-sending"),
                                                {{"server", target}, {"player", context.source().name}}));
        }
    }));
    add(env, "server", "Moves you to another server.", std::move(server));

    add(env, "send", "Sends a player, or everyone, to a server.",
        Node::literal("send").then(Node::argument("player", Kind::Player).then(
            Node::argument("server", Kind::Server).executes([env](const CommandContext& context) {
                const std::string target{context.text("server")};
                const std::string who{context.text("player")};
                if (!hasServer(env, target)) {
                    context.reply(env.messages->command("unknown-server", {{"server", target}}));
                    return;
                }
                std::vector<std::string> names;
                if (who == "all") {
                    for (const auto& entry : env.roster->all())
                        names.push_back(entry.name);
                } else if (const auto online = env.roster->find(who)) {
                    names.push_back(online->name);
                } else {
                    context.reply(env.messages->command("not-online", {{"player", who}}));
                    return;
                }
                for (const std::string& name : names) {
                    context.reply(env.messages->command(sendMessageKey(env.send(name, target), "send-ok"),
                                                        {{"player", name}, {"server", target}}));
                }
            }))));

    add(env, "status", "Shows the proxy's counters.",
        Node::literal("status").executes([env](const CommandContext& context) {
            context.reply(Messages::escape(env.status ? env.status() : std::string{"No status available."}));
        }));

    add(env, "stop", "Stops the proxy.", Node::literal("stop").executes([env, say](const CommandContext& context) {
        say(context, "stopping");
        env.stop();
    }));

    add(env, "reload", "Reloads the configuration and the plugins.",
        Node::literal("reload").executes([env](const CommandContext& context) {
            context.reply(Messages::escape(env.reload ? env.reload() : std::string{"Reload is not available."}));
        }));

    add(env, "plugins", "Lists the loaded plugins.",
        Node::literal("plugins").executes([env](const CommandContext& context) {
            const auto names = env.plugins ? env.plugins() : std::vector<std::string>{};
            context.reply(env.messages->command("plugins", {{"count", std::to_string(names.size())},
                                                            {"plugins", joined(names)}}));
        }),
        {"pl"});

    const auto setOperator = [env](bool on) {
        return [env, on](const CommandContext& context) {
            const std::string name{context.text("player")};
            const auto player = resolvePlayer(env, name);
            if (!player) {
                context.reply(env.messages->command("unknown-player", {{"player", name}}));
                return;
            }
            ensureKnown(env, *player);
            static_cast<void>(env.permissions->setGroup(
                player->name, on ? std::optional{env.permissions->operatorGroup()} : std::nullopt));
            context.reply(env.messages->command(on ? "op-ok" : "deop-ok", {{"player", player->name}}));
        };
    };
    add(env, "op", "Makes a player an operator.",
        Node::literal("op").then(Node::argument("player", Kind::Player).executes(setOperator(true))));
    add(env, "deop", "Takes operator away from a player.",
        Node::literal("deop").then(Node::argument("player", Kind::Player).executes(setOperator(false))));

    Node group = Node::literal("group");
    group.then(Node::literal("list").executes([env](const CommandContext& context) {
        context.reply(env.messages->command("group-list", {{"groups", joined(env.permissions->groupNames())}}));
    }));
    group.then(Node::literal("get").then(Node::argument("player", Kind::Player).executes([env](const CommandContext& context) {
        const std::string name{context.text("player")};
        context.reply(env.messages->command("group-info", {{"player", name},
                                                           {"group", env.permissions->groupOf(name)}}));
    })));
    group.then(Node::literal("members").then(Node::argument("group", Kind::Group).executes([env](const CommandContext& context) {
        const std::string name{context.text("group")};
        if (!env.permissions->groupExists(name)) {
            context.reply(env.messages->command("group-unknown", {{"group", name},
                                                                  {"groups", joined(env.permissions->groupNames())}}));
            return;
        }
        const auto members = env.permissions->membersOf(name);
        context.reply(env.messages->command("group-members", {{"group", name},
                                                              {"count", std::to_string(members.size())},
                                                              {"players", joined(members)}}));
    })));
    group.then(Node::literal("set").then(Node::argument("player", Kind::Player).then(
        Node::argument("group", Kind::Group).executes([env](const CommandContext& context) {
            const std::string name{context.text("player")};
            const std::string target{context.text("group")};
            if (!env.permissions->groupExists(target)) {
                context.reply(env.messages->command("group-unknown", {{"group", target},
                                                                      {"groups", joined(env.permissions->groupNames())}}));
                return;
            }
            const auto player = resolvePlayer(env, name);
            if (!player) {
                context.reply(env.messages->command("unknown-player", {{"player", name}}));
                return;
            }
            ensureKnown(env, *player);
            const std::string lowered = wire::common::placeholder::lowercaseAscii(target);
            const bool toDefault = lowered == env.permissions->defaultGroup();
            static_cast<void>(
                env.permissions->setGroup(player->name, toDefault ? std::nullopt : std::optional{lowered}));
            context.reply(env.messages->command("group-set", {{"player", player->name},
                                                              {"group", lowered}}));
        }))));
    add(env, "group", "Manages permission groups and who is in them.", std::move(group));

    registerAccessCommands(env);
}

} // namespace basalt::proxy::runtime::detail
