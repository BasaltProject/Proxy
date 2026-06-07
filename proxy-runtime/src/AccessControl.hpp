#pragma once

#include "Messages.hpp"
#include "PermissionService.hpp"
#include "PlayerDirectory.hpp"

#include <basalt/wire/config/LiveValue.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace basalt::proxy::runtime::detail {

/// The login gate: bans by player and by address, then the whitelist.
class AccessControl {
public:
    AccessControl(PlayerDirectory& players, PermissionService& permissions, const Messages& messages, bool whitelist);

    /// Reads bans and the whitelist again; every write to them calls this, so logins never touch the database.
    void refresh();

    /// The MiniMessage to disconnect with, or nothing when the player may join.
    [[nodiscard]] std::optional<std::string> check(std::string_view name, std::string_view uuid,
                                                   std::string_view address) const;

    [[nodiscard]] bool whitelistEnabled() const noexcept { return whitelist_.load(std::memory_order_relaxed); }

    void setWhitelist(bool on) noexcept { whitelist_.store(on, std::memory_order_relaxed); }

    /// A player's own ban message, for /ban to kick with.
    [[nodiscard]] std::string banMessage(std::string_view key, std::string_view reason,
                                         std::optional<std::int64_t> expiresAt) const;

private:
    [[nodiscard]] std::optional<std::string> checkBans(std::string_view uuid, std::string_view address) const;

    struct Snapshot {
        std::map<std::string, wire::storage::BanRecord, std::less<>> players;
        std::map<std::string, wire::storage::BanRecord, std::less<>> addresses;
        std::set<std::string, std::less<>> whitelisted;
    };

    PlayerDirectory* players_;
    wire::config::LiveValue<Snapshot> snapshot_{Snapshot{}};
    PermissionService* permissions_;
    const Messages* messages_;
    std::atomic<bool> whitelist_;
};

} // namespace basalt::proxy::runtime::detail
