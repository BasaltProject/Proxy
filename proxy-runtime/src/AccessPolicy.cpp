#include "AccessPolicy.hpp"

#include <algorithm>
#include <regex>

namespace basalt::proxy::runtime::detail {

struct AccessSettings::Filter {
    std::regex pattern;
};

AccessSettings::AccessSettings() = default;
AccessSettings::~AccessSettings() = default;

std::shared_ptr<const AccessSettings> AccessSettings::make(const config::ProxyConfig& config) {
    std::shared_ptr<AccessSettings> settings{new AccessSettings};
    settings->network = config.network;
    settings->general = config.general;
    settings->limits = config.limits;
    for (const auto& entry : config.network.proxyProtocolTrusted)
        if (auto range = config::AddressRange::parse(entry))
            settings->trusted.push_back(*range);
    if (!config.general.usernamePattern.empty())
        settings->usernames_ = std::make_unique<Filter>(Filter{std::regex{config.general.usernamePattern}});
    auto limits = std::make_shared<wire::protocol::PacketLimits>();
    limits->rateLimit = config.limits.packetRateLimit;
    for (const auto& rule : config.limits.packetRules)
        limits->rules.push_back({rule.address.phase, rule.address.id, rule.maxPerSecond,
                                 rule.action == config::PacketRule::Action::kKick});
    settings->packetLimits = std::move(limits);
    return settings;
}

bool AccessSettings::trustsUpstream(std::string_view address) const {
    if (!network.proxyProtocol)
        return false;
    const auto bytes = config::AddressRange::parseAddress(address);
    return bytes && std::ranges::any_of(trusted, [&](const auto& range) { return range.contains(*bytes); });
}

bool AccessSettings::allowsName(std::string_view name) const {
    return !usernames_ || std::regex_match(name.begin(), name.end(), usernames_->pattern);
}

AccessPolicy::AccessPolicy(const config::ProxyConfig& config, Admission& admission)
    : admission(admission), settings_(AccessSettings::make(config)) {}

std::shared_ptr<const AccessSettings> AccessPolicy::settings() const {
    const std::lock_guard lock{mutex_};
    return settings_;
}

void AccessPolicy::apply(const config::ProxyConfig& config) {
    auto fresh = AccessSettings::make(config);
    const std::lock_guard lock{mutex_};
    settings_ = std::move(fresh);
}

} // namespace basalt::proxy::runtime::detail
