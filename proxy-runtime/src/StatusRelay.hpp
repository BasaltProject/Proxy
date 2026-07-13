#pragma once

#include "StatusProbe.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace basalt::proxy::runtime::detail {

/// Short-lived copies of server status answers; never blocks a caller, one thread fetches.
class StatusRelay {
public:
    using Resolve = std::function<std::optional<ProbeTarget>(const std::string& server)>;

    StatusRelay(config::StatusRelayConfig settings, Resolve resolve);
    StatusRelay(const StatusRelay&) = delete;
    StatusRelay& operator=(const StatusRelay&) = delete;

    /// The last answer, which may be a little old while a fresh one is on its way; empty when there is none.
    [[nodiscard]] std::optional<wire::protocol::status::ServerStatusPayload> get(const std::string& server);

private:
    struct Entry {
        wire::protocol::status::ServerStatusPayload payload;
        std::chrono::steady_clock::time_point fetchedAt;
    };

    void work(std::stop_token stop);

    config::StatusRelayConfig settings_;
    Resolve resolve_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::map<std::string, Entry> entries_;
    std::deque<std::string> queue_;
    std::set<std::string> pending_;
    std::jthread worker_;
};

} // namespace basalt::proxy::runtime::detail
