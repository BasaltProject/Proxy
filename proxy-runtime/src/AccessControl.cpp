#include "AccessControl.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace basalt::proxy::runtime::detail {
namespace {

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string dateOf(std::int64_t epochSeconds) {
    const std::time_t time = static_cast<std::time_t>(epochSeconds);
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &time);
#else
    gmtime_r(&time, &parts);
#endif
    std::ostringstream out;
    out << std::put_time(&parts, "%Y-%m-%d %H:%M UTC");
    return out.str();
}

} // namespace

std::string AccessControl::banMessage(std::string_view key, std::string_view reason,
                                      std::optional<std::int64_t> expiresAt) const {
    return messages_->kick(key, {{"reason", std::string{reason}},
                                 {"expires", expiresAt ? dateOf(*expiresAt) : std::string{"never"}}});
}

AccessControl::AccessControl(PlayerDirectory& players, PermissionService& permissions, const Messages& messages,
                             bool whitelist)
    : players_(&players), permissions_(&permissions), messages_(&messages), whitelist_(whitelist) {
    refresh();
}

void AccessControl::refresh() {
    using wire::storage::BanKind;
    Snapshot next;
    players_->locked([&](auto&, auto& access) {
        for (auto& ban : access.bans(BanKind::Player))
            next.players.emplace(ban.subject, std::move(ban));
        for (auto& ban : access.bans(BanKind::Ip))
            next.addresses.emplace(ban.subject, std::move(ban));
        for (const auto& entry : access.whitelist())
            next.whitelisted.insert(entry.uuid);
        return 0;
    });
    snapshot_.replace(std::move(next));
}

std::optional<std::string> AccessControl::checkBans(std::string_view uuid, std::string_view address) const {
    const auto snapshot = snapshot_.current();
    const auto live = [](const wire::storage::BanRecord& ban) {
        return !ban.expiresAtEpochSeconds || *ban.expiresAtEpochSeconds > now();
    };
    if (const auto found = snapshot->players.find(uuid); found != snapshot->players.end() && live(found->second))
        return banMessage("banned", found->second.reason, found->second.expiresAtEpochSeconds);
    if (const auto found = snapshot->addresses.find(address);
        !address.empty() && found != snapshot->addresses.end() && live(found->second))
        return banMessage("banned-ip", found->second.reason, found->second.expiresAtEpochSeconds);
    return std::nullopt;
}

std::optional<std::string> AccessControl::check(std::string_view name, std::string_view uuid,
                                                std::string_view address) const {
    if (auto banned = checkBans(uuid, address))
        return banned;
    if (!whitelistEnabled())
        return std::nullopt;
    const bool listed = snapshot_.current()->whitelisted.contains(uuid);
    if (!listed && !permissions_->isOperator(name))
        return messages_->kick("not-whitelisted");
    return std::nullopt;
}

} // namespace basalt::proxy::runtime::detail
