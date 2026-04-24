#include "Console.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/plugins/PluginHost.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/proxy/runtime/Cli.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/common/logging/LogOutput.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/common/logging/Panic.hpp>
#include <basalt/wire/io/LogFile.hpp>
#include <basalt/wire/io/Platform.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace basalt;
using namespace basalt::proxy::runtime;

namespace {

/// Stable, so orchestrators and scripts can tell a bad command line from a crash.
constexpr int kUsageExitCode = 2;

constexpr std::string_view kIssuesUrl = "https://github.com/BasaltProject/Proxy/issues";

static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> signalStop{false};
std::atomic<bool> signalReload{false};

void stopSignal(int) { signalStop.store(true, std::memory_order_relaxed); }

#if !defined(_WIN32)
void reloadSignal(int) { signalReload.store(true, std::memory_order_relaxed); }
#endif

// Raw string, deliberately unindented: leading spaces in the art are significant.
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

    wire::common::Logger logger{"Proxy"};
    logger.info([&command] { return "Reading configuration from " + command.configDirectory + "."; });
    const auto configuration = proxy::config::load(command.configDirectory);
    if (!configuration) {
        logger.error([&configuration] { return configuration.error(); });
        return 1;
    }
    wire::common::Logger::setDefaultMinLevel(configuration->logging.level);
    logger.setMinLevel(configuration->logging.level);
    switch (configuration->logging.color) {
    case proxy::config::ColorMode::Always:
        wire::common::logOutput::setUseColor(true);
        break;
    case proxy::config::ColorMode::Never:
        wire::common::logOutput::setUseColor(false);
        break;
    case proxy::config::ColorMode::Auto:
        break;
    }
    if (configuration->logging.file)
        static_cast<void>(
            wire::io::logFile::install("./.basalt/logs", configuration->logging.keepDays, cli::versionLine()));
    logger.info([&configuration] {
        return "Configuration validated: " + std::to_string(configuration->backends.size()) + " backend(s).";
    });
    std::atomic<proxy::plugins::PluginHost*> pluginHost{nullptr};
    RuntimeServices services;
    services.playerEvents = [&pluginHost](PlayerEvent event) {
        if (auto* host = pluginHost.load(std::memory_order_acquire))
            host->post(std::move(event));
    };
    auto runtime = ProxyRuntime::create(*configuration, std::move(services));
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
    const std::function<std::string()> reloadConfiguration = [&]() -> std::string {
        auto fresh = proxy::config::load(command.configDirectory);
        if (!fresh)
            return "Reload rejected, nothing changed: " + fresh.error();
        auto applied = runtime->reload(std::move(*fresh));
        if (!applied)
            return "Reload rejected, nothing changed: " + applied.error();
        std::string message =
            "Reloaded " + std::to_string(*applied) + " backend(s); sessions in progress keep their snapshot.";
        if (auto* host = pluginHost.load(std::memory_order_acquire))
            message += " Reloaded " + std::to_string(host->reload()) + " plugin(s).";
        return message;
    };
    std::unique_ptr<proxy::plugins::PluginHost> plugins;
    if (configuration->plugins.enabled) {
        proxy::plugins::PluginSettings settings;
        settings.directory = configuration->plugins.directory;
        settings.database = configuration->plugins.database;
        settings.limits.instructionsPerTurn = configuration->plugins.instructionsPerTurn;
        settings.limits.memoryBytes = configuration->plugins.memoryMib * 1024 * 1024;
        settings.queueSize = configuration->plugins.queueSize;
        proxy::plugins::PluginHooks hooks;
        hooks.servers = [&runtime] { return runtime->backendNames(); };
        hooks.send = [&runtime](std::string_view player, std::string_view backend) {
            return runtime->sendPlayer(player, backend).has_value();
        };
        hooks.log = [](std::string_view plugin, std::string_view line) {
            wire::common::Logger{"Plugin " + std::string{plugin}}.info([&line] { return std::string{line}; });
        };
        auto started = proxy::plugins::PluginHost::start(std::move(settings), std::move(hooks));
        if (!started) {
            logger.error([&started] { return started.error(); });
            return 1;
        }
        plugins = std::move(*started);
        pluginHost.store(plugins.get(), std::memory_order_release);
        logger.info([&plugins] { return "Loaded " + std::to_string(plugins->loaded()) + " plugin(s)."; });
    }
    std::stop_source stopping;
#if !defined(_WIN32)
    const auto oldReload = std::signal(SIGHUP, reloadSignal);
#endif
    std::jthread signals{[&](std::stop_token stop) {
        while (!stop.stop_requested() && !signalStop.load(std::memory_order_relaxed)) {
            if (signalReload.exchange(false, std::memory_order_relaxed))
                logger.info([&] { return reloadConfiguration(); });
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        if (signalStop.load(std::memory_order_relaxed))
            stopping.request_stop();
    }};
    logger.info([&runtime] { return "Listening on port " + std::to_string(runtime->port()) + "."; });
    logger.warn([] {
        return std::string{"Proxy is under development; only verified protocol/backend combinations are supported."};
    });
    std::jthread console{[&](std::stop_token stop) {
        detail::runConsole(*runtime, configuration->backends, stopping, stop, reloadConfiguration,
                           [&pluginHost](std::string_view line) {
                               auto* host = pluginHost.load(std::memory_order_acquire);
                               return host != nullptr && host->command(line);
                           });
    }};
    const auto result = runtime->run(stopping.get_token());
    console.request_stop();
    console.join();
    pluginHost.store(nullptr, std::memory_order_release);
    plugins.reset();
    signals.request_stop();
    signals.join();
    std::signal(SIGINT, oldInterrupt);
    std::signal(SIGTERM, oldTerminate);
#if !defined(_WIN32)
    std::signal(SIGHUP, oldReload);
#endif
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
