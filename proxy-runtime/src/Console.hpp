#pragma once

#include <functional>
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
/// What the console may ask of the Bedrock side, which keeps its own players.
struct BedrockConsole {
    std::function<std::size_t()> playing;
    std::function<bool(std::string_view player, std::string_view backend)> connect;
};

struct ConsoleReply {
    std::string message;
    bool stop = false;
};

[[nodiscard]] ConsoleReply consoleCommand(std::string_view line, ProxyRuntime& runtime,
                                          const std::vector<config::BackendConfig>& backends, std::stop_token stop,
                                          const std::function<std::string()>& reload = {},
                                          const std::function<bool(std::string_view)>& pluginCommand = {},
                                          const BedrockConsole& bedrock = {});
void runConsole(ProxyRuntime& runtime, const std::vector<config::BackendConfig>& backends, std::stop_source& stopping,
                std::stop_token stop, const std::function<std::string()>& reload = {},
                const std::function<bool(std::string_view)>& pluginCommand = {}, const BedrockConsole& bedrock = {});
} // namespace detail
} // namespace basalt::proxy::runtime
