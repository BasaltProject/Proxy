#pragma once

#include "OnlineRoster.hpp"
#include "PlayerDirectory.hpp"

#include <basalt/proxy/runtime/CommandDispatcher.hpp>
#include <basalt/wire/config/LiveValue.hpp>
#include <basalt/wire/config/Permissions.hpp>
#include <basalt/wire/permissions/PermissionChain.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime::detail {

/// Who may do what, asked through the permission chain; groups.toml and players.db are its default provider.
class PermissionService {
public:
    PermissionService(PlayerDirectory& players, wire::config::GroupsConfig initial, const OnlineRoster& roster);

    void reload(wire::config::GroupsConfig groups);

    [[nodiscard]] wire::permissions::PermissionChain& chain() noexcept { return chain_; }

    [[nodiscard]] bool has(const CommandSource& source, std::string_view node) const;

    [[nodiscard]] bool hasPlayer(std::string_view name, std::string_view node) const;

    /// What the session overrides, players.db and groups.toml alone answer; nothing when no rule names the node.
    [[nodiscard]] std::optional<bool> decideByDefault(std::string_view name, std::string_view node) const;

    /// A per-session override of one node for a player, kept until the proxy stops; nullopt removes it.
    void setOverride(std::string_view name, std::string_view node, std::optional<bool> value);

    /// The session override for one node, if the player has one.
    [[nodiscard]] std::optional<bool> overrideOf(std::string_view name, std::string_view node) const;

    /// The patterns the player's group chain and session overrides grant.
    [[nodiscard]] std::vector<std::string> effective(std::string_view name) const;

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
    const OnlineRoster* roster_;
    wire::config::LiveValue<wire::config::GroupsConfig> groups_;
    mutable std::mutex mutex_;
    mutable std::map<std::string, std::optional<std::string>, std::less<>> cache_;
    std::map<std::string, std::map<std::string, bool, std::less<>>, std::less<>> overrides_;
    wire::permissions::PermissionChain chain_;
    wire::permissions::ProviderHandle defaultProvider_;
};

} // namespace basalt::proxy::runtime::detail
