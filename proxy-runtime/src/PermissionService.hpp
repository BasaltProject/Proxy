#pragma once

#include "PlayerDirectory.hpp"

#include <basalt/proxy/runtime/CommandDispatcher.hpp>
#include <basalt/wire/config/LiveValue.hpp>
#include <basalt/wire/config/Permissions.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime::detail {

/// Who may do what: groups.toml says what a group can do, players.db says who is in which group.
class PermissionService {
public:
    PermissionService(PlayerDirectory& players, wire::config::GroupsConfig initial)
        : players_(&players), groups_(std::move(initial)) {}

    void reload(wire::config::GroupsConfig groups);

    /// The console may do everything.
    [[nodiscard]] bool has(const CommandSource& source, std::string_view node) const;

    [[nodiscard]] bool hasPlayer(std::string_view name, std::string_view node) const;

    /// A per-session override of one node for a player, kept until the proxy stops; nullopt removes it.
    void setOverride(std::string_view name, std::string_view node, std::optional<bool> value);

    [[nodiscard]] bool isOperator(std::string_view name) const;

    [[nodiscard]] std::string groupOf(std::string_view name) const;

    /// False when the player has never joined; a group that does not exist is the caller's to check first.
    bool setGroup(std::string_view name, std::optional<std::string> group);

    [[nodiscard]] std::vector<std::string> groupNames() const;

    [[nodiscard]] bool groupExists(std::string_view name) const;

    [[nodiscard]] std::string operatorGroup() const;

    [[nodiscard]] std::string defaultGroup() const;

    [[nodiscard]] std::vector<std::string> membersOf(std::string_view group) const;

    /// Remembers the group of a player who has just logged in.
    void onLogin(std::string_view uuid, std::string_view name);

private:
    [[nodiscard]] std::optional<std::string> storedGroup(std::string_view name) const;

    PlayerDirectory* players_;
    wire::config::LiveValue<wire::config::GroupsConfig> groups_;
    mutable std::mutex mutex_;
    mutable std::map<std::string, std::optional<std::string>, std::less<>> cache_;
    std::map<std::string, std::map<std::string, bool, std::less<>>, std::less<>> overrides_;
};

} // namespace basalt::proxy::runtime::detail
