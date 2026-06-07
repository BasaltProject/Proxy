#include "RelayCounters.hpp"
#include "RouteVersions.hpp"

#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/proxy/runtime/Inspection.hpp>
#include <basalt/wire/multiversion/ProtocolVersions.hpp>
#include <basalt/wire/multiversion/java/JavaTranslators.hpp>

namespace basalt::proxy::runtime {

RelayStats relayStats() noexcept {
    const auto& counters = detail::relayCounters();
    RelayStats stats;
    stats.logins = counters.logins.load(std::memory_order_relaxed);
    stats.backendChanges = counters.backendChanges.load(std::memory_order_relaxed);
    for (std::size_t phase = 0; phase < stats.endedIn.size(); ++phase)
        stats.endedIn[phase] = counters.endedIn[phase].load(std::memory_order_relaxed);
    return stats;
}

std::vector<ProtocolSupport> clientProtocols() {
    std::vector<ProtocolSupport> result;
    for (const std::int32_t protocol : wire::multiversion::java::javaTranslators::supported())
        result.push_back({protocol, wire::multiversion::protocolVersions::describe(protocol),
                          protocol == buildInfo::kProtocolVersion || detail::translatedClientVerified(protocol)});
    return result;
}

std::vector<ProtocolSupport> backendProtocols() {
    std::vector<ProtocolSupport> result;
    const auto add = [&result](std::int32_t protocol, bool verified) {
        result.push_back({protocol, wire::multiversion::protocolVersions::describe(protocol), verified});
    };
    add(buildInfo::kProtocolVersion, true);
    for (const std::int32_t protocol : wire::multiversion::java::javaTranslators::upSupported())
        if (protocol != buildInfo::kProtocolVersion)
            add(protocol, detail::upwardVerified(protocol) || detail::backendVerified(protocol));
    return result;
}

} // namespace basalt::proxy::runtime
