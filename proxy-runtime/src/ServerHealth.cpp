#include "ServerHealth.hpp"

#include "StatusProbe.hpp"

#include <basalt/wire/common/logging/Logger.hpp>

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace basalt::proxy::runtime::detail {
namespace {
bool answers(const HealthTarget& target, const config::HealthConfig& settings, const std::stop_token& stop) {
    const std::chrono::milliseconds timeout{settings.timeoutMs};
    const ProbeTarget probe{target.host, target.port, target.protocol};
    return settings.mode == config::HealthMode::kStatus ? statusPing(probe, timeout, stop).has_value()
                                                        : tcpReachable(probe, timeout, stop);
}
} // namespace

void ServerHealth::run(std::stop_token stop, const config::HealthConfig& settings, const Source& source) {
    const wire::common::Logger log{"Servers"};
    std::map<std::string, std::uint32_t> failures;
    std::map<std::string, bool> reported;
    std::mutex mutex;
    std::condition_variable_any wake;
    while (!stop.stop_requested()) {
        const auto snapshot = source();
        for (std::size_t i = 0; i < snapshot.targets.size() && !stop.stop_requested(); ++i) {
            const auto& target = snapshot.targets[i];
            auto& count = failures[target.name];
            count = answers(target, settings, stop) ? 0 : count + 1;
            const bool down = count >= settings.failuresBeforeDown;
            snapshot.routes->setReachable(i, !down);
            const auto last = reported.find(target.name);
            if (last == reported.end() ? !down : last->second == down)
                continue;
            reported[target.name] = down;
            if (down)
                log.warn([&] { return "Server '" + target.name + "' is not answering and will be skipped."; });
            else
                log.info([&] { return "Server '" + target.name + "' is answering again."; });
        }
        std::unique_lock lock{mutex};
        wake.wait_for(lock, stop, std::chrono::seconds{settings.intervalSeconds}, [] { return false; });
    }
}

} // namespace basalt::proxy::runtime::detail
