#pragma once

#include <basalt/wire/sql/Sqlite.hpp>
#include <basalt/wire/storage/AccessRepository.hpp>
#include <basalt/wire/storage/PlayerIdentityRepository.hpp>
#include <basalt/wire/storage/PluginDataRepository.hpp>

#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace basalt::proxy::runtime::detail {

/// The proxy's own players.db: who has connected, plus the access and plugin tables, behind one lock.
class PlayerDirectory {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<PlayerDirectory>, std::string> open(const std::string& path);

    void recordLogin(std::string_view uuid, std::string_view name, std::optional<std::string_view> address);

    [[nodiscard]] std::optional<std::string> nameByAddress(std::string_view address);

private:
    explicit PlayerDirectory(std::unique_ptr<wire::sql::Database> database)
        : database_(std::move(database)), players_(*database_), access_(*database_), pluginData_(*database_) {}

    std::mutex mutex_;
    std::unique_ptr<wire::sql::Database> database_;
    wire::storage::PlayerIdentityRepository players_;
    wire::storage::AccessRepository access_;
    wire::storage::PluginDataRepository pluginData_;
};

} // namespace basalt::proxy::runtime::detail
