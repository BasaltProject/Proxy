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

PlayerDirectory::~PlayerDirectory() {
    writer_.request_stop();
    wake_.notify_all();
    writer_.join();
}

void PlayerDirectory::post(std::function<void()> work) {
    {
        const std::lock_guard lock{queueMutex_};
        queue_.push_back(std::move(work));
    }
    wake_.notify_one();
}

void PlayerDirectory::drain(std::stop_token stop) {
    while (true) {
        std::function<void()> work;
        {
            std::unique_lock lock{queueMutex_};
            wake_.wait(lock, stop, [this] { return !queue_.empty(); });
            if (stop.stop_requested() || queue_.empty())
                return;
            work = std::move(queue_.front());
            queue_.pop_front();
        }
        work();
    }
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
