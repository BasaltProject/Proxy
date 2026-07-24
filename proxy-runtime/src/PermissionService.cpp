#include "PermissionService.hpp"

#include <basalt/wire/common/placeholder/Placeholders.hpp>

namespace basalt::proxy::runtime::detail {
namespace {
using wire::common::placeholder::lowercaseAscii;
}

void PermissionService::reload(wire::config::GroupsConfig groups) { groups_.replace(std::move(groups)); }

std::optional<std::string> PermissionService::storedGroup(std::string_view name) const {
    const std::string key = lowercaseAscii(name);
    {
        const std::lock_guard lock{mutex_};
        if (const auto found = cache_.find(key); found != cache_.end())
            return found->second;
    }
    auto group = players_->locked([&](auto& players, auto&) -> std::optional<std::string> {
        const auto uuid = players.findUuidByName(name);
        return uuid ? players.groupOf(*uuid) : std::nullopt;
    });
    const std::lock_guard lock{mutex_};
    cache_[key] = group;
    return group;
}

void PermissionService::onLogin(std::string_view uuid, std::string_view name) {
    auto group = players_->locked([&](auto& players, auto&) { return players.groupOf(uuid); });
    const std::lock_guard lock{mutex_};
    cache_[lowercaseAscii(name)] = std::move(group);
}

bool PermissionService::has(const CommandSource& source, std::string_view node) const {
    return source.console || hasPlayer(source.name, node);
}

void PermissionService::setOverride(std::string_view name, std::string_view node, std::optional<bool> value) {
    const std::lock_guard lock{mutex_};
    auto& nodes = overrides_[lowercaseAscii(name)];
    if (value)
        nodes[std::string{node}] = *value;
    else if (const auto found = nodes.find(node); found != nodes.end())
        nodes.erase(found);
}

bool PermissionService::hasPlayer(std::string_view name, std::string_view node) const {
    {
        const std::lock_guard lock{mutex_};
        if (const auto player = overrides_.find(lowercaseAscii(name)); player != overrides_.end())
            if (const auto found = player->second.find(node); found != player->second.end())
                return found->second;
    }
    const auto stored = storedGroup(name);
    return groups_.current()->resolve(stored ? std::optional<std::string_view>{*stored} : std::nullopt, node);
}

std::string PermissionService::groupOf(std::string_view name) const {
    const auto stored = storedGroup(name);
    const auto groups = groups_.current();
    return groups->get(stored ? std::optional<std::string_view>{*stored} : std::nullopt).name;
}

bool PermissionService::isOperator(std::string_view name) const { return groupOf(name) == operatorGroup(); }

bool PermissionService::setGroup(std::string_view name, std::optional<std::string> group) {
    const bool done = players_->locked([&](auto& players, auto&) {
        const auto uuid = players.findUuidByName(name);
        return uuid && players.setGroup(*uuid, group);
    });
    if (done) {
        const std::lock_guard lock{mutex_};
        cache_[lowercaseAscii(name)] = std::move(group);
    }
    return done;
}

std::vector<std::string> PermissionService::groupNames() const { return groups_.current()->names(); }

bool PermissionService::groupExists(std::string_view name) const {
    return groups_.current()->contains(lowercaseAscii(name));
}

std::string PermissionService::operatorGroup() const { return groups_.current()->operatorGroup().name; }

std::string PermissionService::defaultGroup() const { return groups_.current()->defaultGroup().name; }

std::vector<std::string> PermissionService::membersOf(std::string_view group) const {
    std::vector<std::string> names;
    const auto summaries = players_->locked([&](auto& players, auto&) { return players.findInGroup(group); });
    for (const auto& summary : summaries)
        names.push_back(summary.name);
    return names;
}

} // namespace basalt::proxy::runtime::detail
