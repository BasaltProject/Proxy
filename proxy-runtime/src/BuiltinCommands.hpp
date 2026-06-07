#pragma once

#include "AccessControl.hpp"
#include "Messages.hpp"
#include "OnlineRoster.hpp"
#include "PermissionService.hpp"
#include "PlayerDirectory.hpp"

#include <basalt/proxy/runtime/CommandDispatcher.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime::detail {

enum class SendResult { kAccepted, kNotFound, kUnavailable, kBusy, kStopped };

/// What the proxy's own commands act on.
struct CommandEnvironment {
    PlayerDirectory* players = nullptr;
    PermissionService* permissions = nullptr;
    AccessControl* access = nullptr;
    OnlineRoster* roster = nullptr;
    const Messages* messages = nullptr;
    CommandDispatcher* dispatcher = nullptr;
    bool onlineMode = true;
    std::function<std::vector<std::string>()> servers;
    /// Waits, briefly, for the proxy to say whether the move was accepted.
    std::function<SendResult(std::string_view player, std::string_view server)> send;
    std::function<void(std::string_view player, std::string_view miniMessage)> kick;
    std::function<std::string()> reload;
    std::function<std::string()> status;
    std::function<void()> stop;
    std::function<std::vector<std::string>()> plugins;
    std::function<bool()> enforceWhitelist;
};

struct PlayerIdentity {
    std::string name;
    std::string uuid;
};

/// A player the command can name: seen before, online now, or derivable offline.
[[nodiscard]] std::optional<PlayerIdentity> resolvePlayer(const CommandEnvironment& environment, std::string_view name);

void ensureKnown(const CommandEnvironment& environment, const PlayerIdentity& player);

[[nodiscard]] std::string_view sendMessageKey(SendResult result, std::string_view accepted);

[[nodiscard]] bool hasServer(const CommandEnvironment& environment, std::string_view name);

[[nodiscard]] std::string joined(const std::vector<std::string>& names);

void registerBuiltinCommands(const CommandEnvironment& environment);
void registerAccessCommands(const CommandEnvironment& environment);

} // namespace basalt::proxy::runtime::detail
