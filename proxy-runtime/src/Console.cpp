#include "Console.hpp"

#include "RelayCounters.hpp"

#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/io/ConsoleEditor.hpp>

#include <cstdio>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace basalt::proxy::runtime::detail {
namespace {
std::string endedIn(std::size_t phase) {
    return std::to_string(relayCounters().endedIn[phase].load(std::memory_order_relaxed));
}

std::string processUsage() {
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return {};
#if defined(__APPLE__)
    const long peakKib = usage.ru_maxrss / 1024;
#else
    const long peakKib = usage.ru_maxrss;
#endif
    const double cpu = static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                       static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
    char text[96];
    std::snprintf(text, sizeof text, "; cpu seconds: %.1f; peak memory MiB: %ld", cpu, peakKib / 1024);
    return text;
#else
    return {};
#endif
}
} // namespace

std::string statusText(const ProxyRuntime& runtime, std::optional<std::size_t> bedrockPlayers) {
    return "Sessions: " + std::to_string(runtime.sessionCount()) +
           "; retained buffer bytes: " + std::to_string(runtime.bufferedBytes()) +
           "; accepted: " + std::to_string(runtime.acceptedConnections()) +
           "; rejected: " + std::to_string(runtime.rejectedConnections()) +
           "; bytes toward clients: " + std::to_string(runtime.bytesTowardClients()) +
           "; toward servers: " + std::to_string(runtime.bytesTowardBackends()) +
           "; logins: " + std::to_string(relayCounters().logins.load(std::memory_order_relaxed)) +
           "; server changes: " + std::to_string(relayCounters().backendChanges.load(std::memory_order_relaxed)) +
           "; ended in handshake or status: " + endedIn(0) + ", login: " + endedIn(1) +
           ", configuration: " + endedIn(2) + ", play: " + endedIn(3) + processUsage() +
           (bedrockPlayers ? "; Bedrock players: " + std::to_string(*bedrockPlayers) : std::string{}) + ".";
}

void runConsole(CommandService& commands, const CommandDispatcher& dispatcher, std::stop_token stop) {
    const wire::common::Logger logger{"Console"};
    CommandSource console;
    console.name = "CONSOLE";
    console.console = true;
    console.reply = [&logger](std::string_view miniMessage) {
        logger.info([&] { return Messages::plain(miniMessage); });
    };
    wire::io::ConsoleEditorOptions options;
    options.candidates = [&dispatcher, &console](std::string_view line) { return dispatcher.suggest(console, line); };
    wire::io::runConsoleEditor(options, stop, [&](std::string line) {
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return;
        commands.run(console, line);
    });
}

} // namespace basalt::proxy::runtime::detail
