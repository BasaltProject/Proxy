#include "PlayerDirectory.hpp"

#include <basalt/wire/storage/Schema.hpp>

#include <chrono>

namespace basalt::proxy::runtime::detail {

std::expected<std::unique_ptr<PlayerDirectory>, std::string> PlayerDirectory::open(const std::string& path) {
    namespace schema = wire::storage::schema;
    const std::string ddl =
        std::string{schema::identityDdl()} + std::string{schema::accessDdl()} + std::string{schema::pluginDdl()};
    auto database = wire::storage::openPlayerDatabase(path, ddl);
    if (!database)
        return std::unexpected(database.error());
    return std::unique_ptr<PlayerDirectory>{new PlayerDirectory{std::move(*database)}};
}

void PlayerDirectory::recordLogin(std::string_view uuid, std::string_view name,
                                  std::optional<std::string_view> address) {
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
    const std::lock_guard lock{mutex_};
    players_.recordLogin(uuid, name, now.count());
    if (address)
        players_.setLastIp(uuid, std::string{*address});
}

std::optional<std::string> PlayerDirectory::nameByAddress(std::string_view address) {
    const std::lock_guard lock{mutex_};
    return players_.findNameByAddress(address);
}

} // namespace basalt::proxy::runtime::detail
