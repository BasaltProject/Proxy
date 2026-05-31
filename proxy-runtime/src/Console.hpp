#pragma once

#include "CommandService.hpp"
#include "Messages.hpp"

#include <basalt/proxy/runtime/CommandDispatcher.hpp>

#include <optional>
#include <stop_token>
#include <string>

namespace basalt::proxy::runtime {
class ProxyRuntime;

namespace detail {

/// The counters behind the `status` command.
[[nodiscard]] std::string statusText(const ProxyRuntime& runtime, std::optional<std::size_t> bedrockPlayers);

/// Reads the console until it ends or `stop` fires, running each line as the console.
void runConsole(CommandService& commands, const CommandDispatcher& dispatcher, std::stop_token stop);

} // namespace detail
} // namespace basalt::proxy::runtime
