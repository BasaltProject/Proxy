#pragma once

#include <basalt/wire/sql/Sqlite.hpp>
#include <basalt/wire/storage/AccessRepository.hpp>
#include <basalt/wire/storage/PlayerIdentityRepository.hpp>
#include <basalt/wire/storage/PluginDataRepository.hpp>

#include <condition_variable>
#include <deque>
#include <expected>
#include <functional>
#include <thread>
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

    ~PlayerDirectory();

    void recordLogin(std::string_view uuid, std::string_view name, std::optional<std::string_view> address);

    /// Runs `work` on the writer thread, in order, so a session owner never waits on the database.
    void post(std::function<void()> work);

    [[nodiscard]] std::optional<std::string> nameByAddress(std::string_view address);

    /// Runs `work(players, access)` with the database lock held.
    template <typename Work>
    decltype(auto) locked(Work&& work) {
        const std::lock_guard lock{mutex_};
        return work(players_, access_);
    }

private:
    explicit PlayerDirectory(std::unique_ptr<wire::sql::Database> database)
        : database_(std::move(database)), players_(*database_), access_(*database_), pluginData_(*database_),
          writer_([this](std::stop_token stop) { drain(stop); }) {}

    void drain(std::stop_token stop);

    std::mutex mutex_;
    std::mutex queueMutex_;
    std::condition_variable_any wake_;
    std::deque<std::function<void()>> queue_;
    std::unique_ptr<wire::sql::Database> database_;
    wire::storage::PlayerIdentityRepository players_;
    wire::storage::AccessRepository access_;
    wire::storage::PluginDataRepository pluginData_;
    std::jthread writer_;
};

} // namespace basalt::proxy::runtime::detail
