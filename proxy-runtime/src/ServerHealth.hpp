#pragma once

#include "BackendRoutes.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>

#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace basalt::proxy::runtime::detail {

struct HealthTarget {
    std::string name;
    std::string host;
    std::uint16_t port = 0;
    std::int32_t protocol = 0;
};

struct HealthSnapshot {
    std::vector<HealthTarget> targets;
    std::shared_ptr<const BackendRoutes> routes;
};

/// Background reachability checks. Sleeps between rounds; owns no state beyond failure counters.
class ServerHealth {
public:
    using Source = std::function<HealthSnapshot()>;

    static void run(std::stop_token stop, const config::HealthConfig& settings, const Source& source);
};

} // namespace basalt::proxy::runtime::detail
