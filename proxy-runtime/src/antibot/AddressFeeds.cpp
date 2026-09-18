#include "AddressFeeds.hpp"

#include <basalt/wire/auth/HttpsClient.hpp>
#include <basalt/wire/common/logging/Logger.hpp>

#include <algorithm>
#include <chrono>

namespace basalt::proxy::runtime::antibot {
namespace {
constexpr std::size_t kListMaxBytes = 4UL * 1024UL * 1024UL;
constexpr std::size_t kLookupMaxBytes = 64UL * 1024UL;
constexpr std::size_t kMaxQueued = 256;
constexpr std::size_t kMaxCached = 50000;
constexpr auto kListTimeout = std::chrono::seconds{20};
constexpr auto kLookupTimeout = std::chrono::seconds{5};

bool ipv4(const OriginKey& origin) {
    return std::all_of(origin.bytes.begin(), origin.bytes.begin() + 10, [](auto v) { return v == 0; }) &&
           origin.bytes[10] == 0xff && origin.bytes[11] == 0xff;
}

std::string dotted(const OriginKey& origin) {
    return std::to_string(origin.bytes[12]) + "." + std::to_string(origin.bytes[13]) + "." +
           std::to_string(origin.bytes[14]) + "." + std::to_string(origin.bytes[15]);
}
} // namespace

AddressFeeds::AddressFeeds(ConfigSource source)
    : source_(std::move(source)), imported_(std::make_shared<const std::vector<config::AddressRange>>()),
      worker_([this](std::stop_token stop) { run(std::move(stop)); }) {}

AddressFeeds::~AddressFeeds() {
    worker_.request_stop();
    wake_.notify_all();
}

bool AddressFeeds::imported(const OriginKey& origin) const {
    std::shared_ptr<const std::vector<config::AddressRange>> ranges;
    {
        const std::lock_guard lock{mutex_};
        ranges = imported_;
    }
    return std::ranges::any_of(*ranges, [&](const auto& range) { return range.contains(origin.bytes); });
}

std::optional<bool> AddressFeeds::proxyKnown(const OriginKey& origin, Clock::time_point now) {
    if (!ipv4(origin))
        return std::nullopt;
    const std::lock_guard lock{mutex_};
    if (const auto found = cache_.find(origin); found != cache_.end() && found->second.expires > now)
        return found->second.proxy;
    if (queue_.size() < kMaxQueued && std::ranges::find(queue_, origin) == queue_.end()) {
        queue_.push_back(origin);
        wake_.notify_one();
    }
    return std::nullopt;
}

void AddressFeeds::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        const auto config = source_();
        if (config && config->enabled) {
            refreshLists(*config, stop);
            lookups(*config, stop);
        }
        std::unique_lock lock{mutex_};
        wake_.wait_for(lock, stop, std::chrono::seconds{1}, [] { return false; });
    }
}

void AddressFeeds::refreshLists(const config::AntibotConfig& config, std::stop_token stop) {
    const auto now = Clock::now();
    if (config.blockedUrls.empty() || now < nextRefresh_)
        return;
    nextRefresh_ = now + std::chrono::minutes{config.listRefreshMinutes};
    auto merged = std::make_shared<std::vector<config::AddressRange>>();
    for (const auto& url : config.blockedUrls) {
        wire::auth::BoundedHttpsOptions options;
        options.deadline = Clock::now() + kListTimeout;
        options.stop = stop;
        const auto response = wire::auth::httpsGetBounded(url, kListMaxBytes, options);
        if (!response || !response->ok()) {
            wire::common::Logger{"Antibot"}.warn([&] { return "Could not download the block list " + url + "."; });
            continue;
        }
        const auto parsed = config::AddressRange::parseList(response->text());
        merged->insert(merged->end(), parsed.begin(), parsed.end());
    }
    const std::lock_guard lock{mutex_};
    imported_ = std::move(merged);
}

void AddressFeeds::lookups(const config::AntibotConfig& config, std::stop_token stop) {
    if (!config.antiVpnEnabled || config.antiVpnUrl.empty())
        return;
    while (!stop.stop_requested()) {
        OriginKey origin;
        {
            const std::lock_guard lock{mutex_};
            const auto now = Clock::now();
            if (queue_.empty())
                return;
            if (now - windowStart_ >= std::chrono::minutes{1}) {
                windowStart_ = now;
                windowCount_ = 0;
            }
            if (windowCount_ >= config.antiVpnQueriesPerMinute)
                return;
            ++windowCount_;
            origin = queue_.back();
            queue_.pop_back();
        }
        auto url = config.antiVpnUrl;
        if (const auto at = url.find("{ip}"); at != std::string::npos)
            url.replace(at, 4, dotted(origin));
        wire::auth::BoundedHttpsOptions options;
        options.deadline = Clock::now() + kLookupTimeout;
        options.stop = stop;
        const auto response = wire::auth::httpsGetBounded(url, kLookupMaxBytes, options);
        if (!response || !response->ok())
            continue;
        const bool proxy = response->text().find(config.antiVpnMatch) != std::string::npos;
        const std::lock_guard lock{mutex_};
        if (cache_.size() >= kMaxCached)
            cache_.clear();
        cache_[origin] = {proxy, Clock::now() + std::chrono::hours{config.antiVpnCacheHours}};
    }
}

} // namespace basalt::proxy::runtime::antibot
