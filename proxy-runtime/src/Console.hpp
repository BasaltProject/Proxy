#pragma once

#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::config {
struct BackendConfig;
}

namespace basalt::proxy::runtime {
class ProxyRuntime;

namespace detail {
struct ConsoleReply {
    std::string message;
    bool stop = false;
};

[[nodiscard]] ConsoleReply consoleCommand(std::string_view line, ProxyRuntime& runtime,
                                          const std::vector<config::BackendConfig>& backends, std::stop_token stop);
void runConsole(ProxyRuntime& runtime, const std::vector<config::BackendConfig>& backends, std::stop_source& stopping,
                std::stop_token stop);
} // namespace detail
} // namespace basalt::proxy::runtime
