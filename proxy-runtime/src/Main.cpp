#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/proxy/runtime/Cli.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/common/logging/LogOutput.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/common/logging/Panic.hpp>
#include <basalt/wire/io/Platform.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace basalt;
using namespace basalt::proxy::runtime;

namespace {

/// Stable, so orchestrators and scripts can tell a bad command line from a crash.
constexpr int kUsageExitCode = 2;

/// Where a crash report goes. The proxy's own: the server has its own tracker and its own half of
/// the code.
constexpr std::string_view kIssuesUrl = "https://github.com/BasaltProject/Proxy/issues";

static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> signalStop{false};

void stopSignal(int) { signalStop.store(true, std::memory_order_relaxed); }

// Raw string, and deliberately not indented: the art is full of backslashes, and every leading
// space in it is significant.
constexpr std::string_view kBanner = R"(
  ____                  _ _     _____
 |  _ \                | | |   |  __ \
 | |_) | __ _ ___  __ _| | |_  | |__) | __ _____  ___   _
 |  _ < / _` / __|/ _` | | __| |  ___/ '__/ _ \ \/ / | | |
 | |_) | (_| \__ \ (_| | | |_  | |   | | | (_) >  <| |_| |
 |____/ \__,_|___/\__,_|_|\__| |_|   |_|  \___/_/\_\\__, |
                                                     __/ |
                                                    |___/
)";

int run(const cliCommand::Run& command) {
    // Escape codes are a terminal affordance; piped or redirected output gets none.
    wire::common::logOutput::setUseColor(wire::io::isConsoleInteractive());
    std::puts(std::string{kBanner}.c_str());
    std::puts(cli::versionLine().c_str());

    const wire::common::Logger logger{"Proxy"};
    logger.info([&command] { return "Reading configuration from " + command.configDirectory + "."; });
    const auto configuration = proxy::config::load(command.configDirectory);
    if (!configuration) {
        logger.error([&configuration] { return configuration.error(); });
        return 1;
    }
    logger.info([&configuration] {
        return "Configuration validated: " + std::to_string(configuration->backends.size()) + " backend(s).";
    });
    auto runtime = ProxyRuntime::create(*configuration);
    if (!runtime) {
        logger.error([&runtime] { return runtime.error(); });
        return 1;
    }
    signalStop.store(false);
    const auto oldInterrupt = std::signal(SIGINT, stopSignal);
    const auto oldTerminate = std::signal(SIGTERM, stopSignal);
    if (oldInterrupt == SIG_ERR || oldTerminate == SIG_ERR) {
        logger.error([] { return std::string{"Could not install shutdown signal handlers."}; });
        return 1;
    }
    std::stop_source stopping;
    std::jthread signals{[&](std::stop_token stop) {
        while (!stop.stop_requested() && !signalStop.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        if (signalStop.load(std::memory_order_relaxed))
            stopping.request_stop();
    }};
    logger.info([&runtime] { return "Listening on port " + std::to_string(runtime->port()) + "."; });
    logger.warn([] { return std::string{"Status is available; player routing is not available in this build."}; });
    const auto result = runtime->run(stopping.get_token());
    signals.request_stop();
    signals.join();
    std::signal(SIGINT, oldInterrupt);
    std::signal(SIGTERM, oldTerminate);
    if (!result) {
        logger.error([&result] { return result.error(); });
        return 1;
    }
    logger.info([] { return std::string{"Proxy stopped."}; });
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    wire::common::panic::install("Basalt Proxy", std::string{buildInfo::kVersion}, std::string{kIssuesUrl});

    const std::vector<std::string_view> args{argv + 1, argv + argc};
    const CliCommand command = cli::parse(args);
    if (std::holds_alternative<cliCommand::ShowHelp>(command)) {
        std::puts(cli::helpText().c_str());
        return 0;
    }
    if (std::holds_alternative<cliCommand::ShowVersion>(command)) {
        std::puts(cli::versionLine().c_str());
        return 0;
    }
    if (const auto* invalid = std::get_if<cliCommand::Invalid>(&command); invalid != nullptr) {
        std::puts(invalid->message.c_str());
        return kUsageExitCode;
    }
    return run(std::get<cliCommand::Run>(command));
}
