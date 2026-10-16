#include "Console.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/io/ConsoleInput.hpp>

#include <array>
#include <chrono>
#include <thread>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::string_view kHelp = "Commands: help, servers, status, reload, send <player> <backend>, stop.";
}

ConsoleReply consoleCommand(std::string_view line, ProxyRuntime& runtime,
                            const std::vector<config::BackendConfig>& backends, std::stop_token stop,
                            const std::function<std::string()>& reload) {
    static_cast<void>(backends);
    if (line.size() > 4096 || line.find('\0') != std::string_view::npos)
        return {"Invalid console command."};
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    line.remove_prefix(first);
    if (line.starts_with('/'))
        line.remove_prefix(1);
    std::array<std::string_view, 4> words{};
    std::size_t count = 0;
    while (!line.empty()) {
        const auto begin = line.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos)
            break;
        line.remove_prefix(begin);
        const auto end = line.find_first_of(" \t\r\n");
        if (count == words.size())
            return {std::string{kHelp}};
        words[count++] = line.substr(0, end);
        if (end == std::string_view::npos)
            break;
        line.remove_prefix(end);
    }
    if (count == 1) {
        if (words[0] == "help")
            return {std::string{kHelp}};
        if (words[0] == "stop")
            return {"Stopping proxy.", true};
        if (words[0] == "status")
            return {"Sessions: " + std::to_string(runtime.sessionCount()) +
                    "; retained buffer bytes: " + std::to_string(runtime.bufferedBytes()) +
                    "; accepted: " + std::to_string(runtime.acceptedConnections()) +
                    "; rejected: " + std::to_string(runtime.rejectedConnections()) + "."};
        if (words[0] == "servers") {
            std::string names = "Backends:";
            for (const auto& name : runtime.backendNames())
                names += " " + name;
            return {std::move(names)};
        }
        if (words[0] == "reload")
            return {reload ? reload() : "Reload is not available."};
    }
    if (count != 3 || words[0] != "send")
        return {std::string{kHelp}};
    auto request = runtime.sendPlayer(words[1], words[2]);
    if (!request) {
        switch (request.error()) {
        case BackendChangeError::kNotRunning:
            return {"Proxy is not running."};
        case BackendChangeError::kInvalidPlayer:
            return {"Invalid player name."};
        case BackendChangeError::kUnknownBackend:
            return {"Unknown backend."};
        case BackendChangeError::kBusy:
            return {"Control queue or buffer budget is full."};
        }
    }
    while (!stop.stop_requested()) {
        if (const auto result = request->result()) {
            switch (*result) {
            case BackendChangeResult::kAccepted:
                return {"Backend change preparation accepted for " + std::string{words[1]} + "."};
            case BackendChangeResult::kPlayerNotFound:
                return {"Player not found (names are case-sensitive)."};
            case BackendChangeResult::kUnavailable:
                return {
                    "Backend change unavailable: session is not in play, destination is current, or change is busy."};
            case BackendChangeResult::kStopped:
                return {"Proxy stopped before the request completed."};
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    return {"Console request interrupted."};
}

void runConsole(ProxyRuntime& runtime, const std::vector<config::BackendConfig>& backends, std::stop_source& stopping,
                std::stop_token stop, const std::function<std::string()>& reload) {
    const wire::common::Logger logger{"Console"};
    wire::io::ConsoleInput input;
    while (!stop.stop_requested() && !stopping.stop_requested()) {
        const auto line = input.readLine(stop);
        if (!line) {
            if (line.error() == wire::io::ConsoleInputError::kTooLong) {
                logger.warn([] { return std::string{"Console command exceeds 4096 bytes; line discarded."}; });
                continue;
            }
            break;
        }
        const auto reply = consoleCommand(*line, runtime, backends, stop, reload);
        if (!reply.message.empty())
            logger.info([&] { return reply.message; });
        if (reply.stop) {
            stopping.request_stop();
            break;
        }
    }
}

} // namespace basalt::proxy::runtime::detail
