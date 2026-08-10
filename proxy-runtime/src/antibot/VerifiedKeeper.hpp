#pragma once

#include "Antibot.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace basalt::proxy::runtime::antibot {

/// Background upkeep of the verified list: bounded purge of the stored rows and the shared-directory sync.
class VerifiedKeeper {
public:
    explicit VerifiedKeeper(Antibot& antibot);
    ~VerifiedKeeper();
    VerifiedKeeper(const VerifiedKeeper&) = delete;
    VerifiedKeeper& operator=(const VerifiedKeeper&) = delete;

    void usePurge(decltype(VerifiedStore::purge) purge);
    /// Wakes the loop now, so an attack that just began is watched until it ends.
    void poke();

private:
    void run(std::stop_token stop);
    void publish(const config::AntibotConfig& config, const std::string& id);
    void collect(const config::AntibotConfig& config, const std::string& id);

    Antibot& antibot_;
    decltype(VerifiedStore::purge) purge_;
    std::atomic<bool> poked_{false};
    std::map<std::string, std::filesystem::file_time_type> seen_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::jthread worker_;
};

} // namespace basalt::proxy::runtime::antibot
