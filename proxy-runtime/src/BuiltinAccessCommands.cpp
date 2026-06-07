#include "BuiltinCommands.hpp"

#include <basalt/wire/io/TcpSocket.hpp>

#include <algorithm>
#include <chrono>

namespace basalt::proxy::runtime::detail {
namespace {

using Node = CommandNode;
using Kind = ArgumentKind;

void add(const CommandEnvironment& environment, std::string name, std::string description, Node root) {
    CommandRegistration registration;
    registration.aliases = {"basalt:" + name};
    registration.permission = "basalt.command." + name;
    registration.root = std::move(root);
    registration.description = std::move(description);
    registration.owner = "basalt";
    static_cast<void>(environment.dispatcher->add(std::move(registration)));
}

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool isAddress(std::string_view text) { return wire::io::TcpAddress::literal(std::string{text}, 0).has_value(); }

void kickNonWhitelisted(const CommandEnvironment& env) {
    if (!env.enforceWhitelist || !env.enforceWhitelist() || !env.access->whitelistEnabled())
        return;
    for (const auto& entry : env.roster->all()) {
        const bool listed = env.players->locked([&](auto&, auto& access) { return access.isWhitelisted(entry.uuid); });
        if (!listed && !env.permissions->isOperator(entry.name))
            env.kick(entry.name, env.messages->kick("not-whitelisted"));
    }
}

} // namespace

void registerAccessCommands(const CommandEnvironment& env) {
    Node whitelist = Node::literal("whitelist");
    whitelist.then(Node::literal("on").executes([env](const CommandContext& context) {
        env.access->setWhitelist(true);
        context.reply(env.messages->command("whitelist-on"));
        kickNonWhitelisted(env);
    }));
    whitelist.then(Node::literal("off").executes([env](const CommandContext& context) {
        env.access->setWhitelist(false);
        context.reply(env.messages->command("whitelist-off"));
    }));
    whitelist.then(Node::literal("list").executes([env](const CommandContext& context) {
        std::vector<std::string> names;
        for (const auto& entry : env.players->locked([&](auto&, auto& access) { return access.whitelist(); }))
            names.push_back(entry.name);
        context.reply(env.messages->command("whitelist-list",
                                            {{"count", std::to_string(names.size())}, {"players", joined(names)}}));
    }));
    whitelist.then(
        Node::literal("add").then(Node::argument("player", Kind::Player).executes([env](const CommandContext& context) {
            const std::string name{context.text("player")};
            const auto player = resolvePlayer(env, name);
            if (!player) {
                context.reply(env.messages->command("unknown-player", {{"player", name}}));
                return;
            }
            const bool added = env.players->locked([&](auto&, auto& access) {
                if (access.isWhitelisted(player->uuid))
                    return false;
                access.addToWhitelist({.uuid = player->uuid, .name = player->name, .addedAtEpochSeconds = now()});
                return true;
            });
            env.access->refresh();
            context.reply(
                env.messages->command(added ? "whitelist-added" : "whitelist-already", {{"player", player->name}}));
        })));
    whitelist.then(Node::literal("remove").then(
        Node::argument("player", Kind::Player).executes([env](const CommandContext& context) {
            const std::string name{context.text("player")};
            const bool removed = env.players->locked([&](auto&, auto& access) {
                const auto entry = access.whitelistedByName(name);
                return entry && access.removeFromWhitelist(entry->uuid);
            });
            env.access->refresh();
            context.reply(
                env.messages->command(removed ? "whitelist-removed" : "whitelist-not-listed", {{"player", name}}));
            if (removed)
                kickNonWhitelisted(env);
        })));
    add(env, "whitelist", "Manages who may join while the whitelist is on.", std::move(whitelist));

    const CommandExecutor banner = [env](const CommandContext& context) {
        const std::string target{context.text("target")};
        const std::string reason = context.has("reason") ? std::string{context.text("reason")}
                                                         : Messages::plain(env.messages->command("default-reason"));
        const bool ip = isAddress(target);
        std::string subject = target;
        std::string name;
        if (!ip) {
            const auto player = resolvePlayer(env, target);
            if (!player) {
                context.reply(env.messages->command("unknown-player", {{"player", target}}));
                return;
            }
            subject = player->uuid;
            name = player->name;
        }
        const auto kind = ip ? wire::storage::BanKind::Ip : wire::storage::BanKind::Player;
        const bool banned = env.players->locked([&](auto&, auto& access) {
            if (access.banOf(subject, kind))
                return false;
            access.ban({.subject = subject,
                        .kind = kind,
                        .name = name.empty() ? std::nullopt : std::optional{name},
                        .reason = reason,
                        .source = context.source().name,
                        .createdAtEpochSeconds = now()});
            return true;
        });
        env.access->refresh();
        if (!banned) {
            context.reply(env.messages->command("ban-already", {{"player", target}}));
            return;
        }
        context.reply(env.messages->command("ban-ok", {{"player", target}, {"reason", reason}}));
        for (const auto& entry : env.roster->all())
            if (ip ? false : entry.uuid == subject)
                env.kick(entry.name, env.access->banMessage("banned", reason, std::nullopt));
    };
    add(env, "ban", "Bans a player or an address.",
        Node::literal("ban").then(Node::argument("target", ArgumentKind::Word)
                                      .executes(banner)
                                      .then(Node::argument("reason", ArgumentKind::Text, true).executes(banner))));
    add(env, "pardon", "Lifts a ban.",
        Node::literal("pardon").then(
            Node::argument("target", ArgumentKind::Word).executes([env](const CommandContext& context) {
                const std::string target{context.text("target")};
                const bool ip = isAddress(target);
                std::string subject = target;
                if (!ip) {
                    const auto player = resolvePlayer(env, target);
                    if (!player) {
                        context.reply(env.messages->command("unknown-player", {{"player", target}}));
                        return;
                    }
                    subject = player->uuid;
                }
                const bool lifted = env.players->locked([&](auto&, auto& access) {
                    return access.pardon(subject, ip ? wire::storage::BanKind::Ip : wire::storage::BanKind::Player);
                });
                env.access->refresh();
                context.reply(env.messages->command(lifted ? "pardon-ok" : "pardon-not-banned", {{"player", target}}));
            })));
    add(env, "banlist", "Lists who is banned.", Node::literal("banlist").executes([env](const CommandContext& context) {
        std::vector<std::string> names;
        env.players->locked([&](auto&, auto& access) {
            for (const auto& ban : access.bans(wire::storage::BanKind::Player))
                names.push_back(ban.name.value_or(ban.subject));
            for (const auto& ban : access.bans(wire::storage::BanKind::Ip))
                names.push_back(ban.subject);
            return 0;
        });
        context.reply(
            env.messages->command("banlist", {{"count", std::to_string(names.size())}, {"players", joined(names)}}));
    }));

    const CommandExecutor kicker = [env](const CommandContext& context) {
        const std::string name{context.text("player")};
        const auto online = env.roster->find(name);
        if (!online) {
            context.reply(env.messages->command("not-online", {{"player", name}}));
            return;
        }
        const std::string reason = context.has("reason") ? std::string{context.text("reason")}
                                                         : Messages::plain(env.messages->command("default-reason"));
        env.kick(online->name, env.messages->kick("kicked", {{"reason", reason}}));
        context.reply(env.messages->command("kick-ok", {{"player", online->name}}));
    };
    add(env, "kick", "Disconnects a player.",
        Node::literal("kick").then(Node::argument("player", Kind::Player)
                                       .executes(kicker)
                                       .then(Node::argument("reason", ArgumentKind::Text, true).executes(kicker))));
}

} // namespace basalt::proxy::runtime::detail
