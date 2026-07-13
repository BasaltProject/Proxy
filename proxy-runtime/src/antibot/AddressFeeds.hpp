#pragma once

#include "Antibot.hpp"

#include <basalt/proxy/config/AddressRange.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace basalt::proxy::runtime::antibot {

/// Bounded background downloads for the antibot: URL block lists and anti-VPN lookups.
class AddressFeeds {
public:
    using ConfigSource = std::function<std::shared_ptr<const config::AntibotConfig>()>;

    explicit AddressFeeds(ConfigSource source);
    ~AddressFeeds();
    AddressFeeds(const AddressFeeds&) = delete;
    AddressFeeds& operator=(const AddressFeeds&) = delete;

    [[nodiscard]] bool imported(const OriginKey& origin) const;
    /// The cached answer for an IPv4 origin, or nothing while it is unknown (a lookup is then queued).
    [[nodiscard]] std::optional<bool> proxyKnown(const OriginKey& origin, Clock::time_point now);

private:
    struct Entry {
        bool proxy = false;
        Clock::time_point expires;
    };

    void run(std::stop_token stop);
    void refreshLists(const config::AntibotConfig& config, std::stop_token stop);
    void lookups(const config::AntibotConfig& config, std::stop_token stop);

    ConfigSource source_;
    mutable std::mutex mutex_;
    std::shared_ptr<const std::vector<config::AddressRange>> imported_;
    std::unordered_map<OriginKey, Entry, OriginHash> cache_;
    std::vector<OriginKey> queue_;
    Clock::time_point nextRefresh_{};
    Clock::time_point windowStart_{};
    std::int32_t windowCount_ = 0;
    std::condition_variable_any wake_;
    std::jthread worker_;
};

} // namespace basalt::proxy::runtime::antibot
