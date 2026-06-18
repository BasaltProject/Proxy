#include "AccessControl.hpp"
#include "BackendRoutes.hpp"
#include "BedrockStatus.hpp"
#include "BuiltinCommands.hpp"
#include "CommandService.hpp"
#include "Console.hpp"
#include "ForwardingKey.hpp"
#include "Messages.hpp"
#include "NativeForwarding.hpp"
#include "OnlineRoster.hpp"
#include "PermissionService.hpp"
#include "PlayerDirectory.hpp"
#include "QueryServer.hpp"
#include "ServerListService.hpp"
#include "UiService.hpp"
#include "bedrock/BedrockLanguage.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/config/RemoteConfig.hpp>
#include <basalt/proxy/plugins/PluginHost.hpp>
#include <basalt/proxy/remote/RemoteApi.hpp>

#include <cstdlib>
#if defined(BASALT_HAS_SHIM)
#include <basalt/proxy/bungee/BungeeAdapter.hpp>
#include <basalt/proxy/velocity/VelocityAdapter.hpp>
#endif
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/proxy/runtime/Cli.hpp>
#include <basalt/proxy/runtime/CommandDispatcher.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/proxy/runtime/UpdateChecker.hpp>
#include <basalt/wire/common/logging/LogOutput.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/common/logging/Panic.hpp>
#include <basalt/wire/config/BasicPlaceholders.hpp>
#include <basalt/wire/io/LogFile.hpp>
#include <basalt/wire/io/Platform.hpp>
#include <basalt/wire/plugins/PacketNaming.hpp>
#include <basalt/wire/plugins/PacketRegistry.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
    const auto remoteConfiguration = proxy::config::loadRemote(command.configDirectory);
    if (!remoteConfiguration) {
        logger.error([&remoteConfiguration] { return remoteConfiguration.error(); });
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
    std::atomic<detail::BedrockStatus*> bedrockPlayers{nullptr};
    std::atomic<ProxyRuntime*> runtimeRef{nullptr};
    auto players = detail::PlayerDirectory::open(".basalt/players.db");
    if (!players) {
        logger.error([&players] { return players.error(); });
        return 1;
    }
    auto serverList = detail::ServerListService::create(
        command.configDirectory,
        {.slots = static_cast<std::int32_t>(configuration->runtime.maxSessions), .players = players->get()});
    if (!serverList) {
        logger.error([&serverList] { return serverList.error(); });
        return 1;
    }
    auto ui = detail::UiService::create(command.configDirectory, (*serverList)->placeholders());
    if (!ui) {
        logger.error([&ui] { return ui.error(); });
        return 1;
    }
    (*serverList)->setLoading(true);
    auto groups = proxy::config::loadGroups(command.configDirectory);
    auto commandsConfig = proxy::config::loadCommands(command.configDirectory);
    auto messageSet = proxy::config::loadMessages(command.configDirectory);
    for (const std::string* problem :
         {groups ? nullptr : &groups.error(), commandsConfig ? nullptr : &commandsConfig.error(),
          messageSet ? nullptr : &messageSet.error()}) {
        if (problem != nullptr) {
            logger.error([problem] { return *problem; });
            return 1;
        }
    }
    detail::OnlineRoster roster;
    detail::PermissionService permissions{**players, std::move(*groups)};
    detail::Messages messages{
        std::move(*messageSet),
        wire::config::BasicFacts{
            .versionName = std::string{buildInfo::kMinecraftVersion},
            .protocolVersion = buildInfo::kProtocolVersion,
            .online = [&runtimeRef]() -> std::int32_t {
                auto* live = runtimeRef.load(std::memory_order_acquire);
                return live != nullptr ? static_cast<std::int32_t>(live->sessionCount()) : 0;
            },
            .maxPlayers = [slots = static_cast<std::int32_t>(configuration->runtime.maxSessions)] { return slots; }}};
    detail::AccessControl access{**players, permissions, messages, configuration->access.whitelist};
    std::atomic<bool> enforceWhitelist{configuration->access.enforceWhitelist};
    bool whitelistSetting = configuration->access.whitelist;
    const auto tellPlayer = [&runtimeRef, &bedrockPlayers](std::string_view player, std::string_view text) {
        if (auto* live = runtimeRef.load(std::memory_order_acquire))
            static_cast<void>(live->tellPlayer(player, text));
        if (auto* bedrockSide = bedrockPlayers.load(std::memory_order_acquire))
            bedrockSide->tell(player, text);
    };
    const auto kickPlayer = [&runtimeRef, &bedrockPlayers](std::string_view player, std::string_view reason) {
        if (auto* live = runtimeRef.load(std::memory_order_acquire))
            static_cast<void>(live->kickPlayer(player, reason));
        if (auto* bedrockSide = bedrockPlayers.load(std::memory_order_acquire))
            bedrockSide->kick(player, reason);
    };
    const auto backendNames = [&runtimeRef]() -> std::vector<std::string> {
        auto* live = runtimeRef.load(std::memory_order_acquire);
        return live != nullptr ? live->backendNames() : std::vector<std::string>{};
    };
    CommandDispatcher dispatcher{
        [&permissions](const CommandSource& source, std::string_view node) { return permissions.has(source, node); },
        CommandSuggestions{[&roster] {
                               std::vector<std::string> names;
                               for (const auto& entry : roster.all())
                                   names.push_back(entry.name);
                               return names;
                           },
                           backendNames, [&permissions] { return permissions.groupNames(); }}};
    dispatcher.renderCustomWith([&messages](std::string_view text) { return messages.fill(text); });
    dispatcher.configure(std::make_shared<const wire::config::CommandsConfig>(std::move(*commandsConfig)));
    detail::CommandService commandService{dispatcher, messages};
    const wire::common::Logger consoleLogger{"Console"};
    const CommandSource consoleSource{"CONSOLE", true, [&consoleLogger](std::string_view miniMessage) {
                                          consoleLogger.info([&] { return detail::Messages::plain(miniMessage); });
                                      }};
    const auto playerSource = [&tellPlayer](std::string_view name) {
        return CommandSource{
            std::string{name}, false,
            [&tellPlayer, player = std::string{name}](std::string_view text) { tellPlayer(player, text); }};
    };
    std::stop_source stopping;
    RuntimeServices services;
    std::shared_ptr<wire::plugins::PacketRegistry> packetRegistry;
    if (configuration->plugins.enabled) {
        packetRegistry = std::make_shared<wire::plugins::PacketRegistry>(
            [&pluginHost](const void* owner, const wire::lua::LuaRef& function, wire::plugins::PluginPacket& packet) {
                auto* host = pluginHost.load(std::memory_order_acquire);
                return host == nullptr || host->deliverPacket(owner, function, packet);
            },
            [](const wire::plugins::PacketAddress& address) { return wire::plugins::packetNaming::of(address); });
        services.packets = packetRegistry;
        services.packetMaxBytes = configuration->plugins.packetMaxKib * 1024;
    }
    services.status = [&serverList](std::int32_t protocol, std::string_view host, std::string_view address) {
        return (*serverList)->statusFor(protocol, host, address);
    };
    const auto playerCommandSpecs = [&dispatcher, playerSource](std::string_view player) {
        return dispatcher.specs(playerSource(player));
    };
    const auto playerCommand = [&commandService, playerSource](std::string_view player, std::string_view line) {
        return commandService.submit(playerSource(player), std::string{line});
    };
    services.playerCommandSpecs = playerCommandSpecs;
    services.playerCommand = playerCommand;
    services.commandSuggestions = [&dispatcher,
                                   playerSource](std::string_view player,
                                                 std::string_view line) -> std::optional<std::vector<std::string>> {
        const CommandSource source = playerSource(player);
        if (!dispatcher.available(source, line))
            return std::nullopt;
        return dispatcher.suggest(source, line);
    };
    services.isOperator = [&permissions](std::string_view player) { return permissions.isOperator(player); };
    services.kickText = [&messages](std::string_view key) { return messages.kick(key); };
    services.playerChat = [&pluginHost](std::string_view player, std::string_view message) {
        if (auto* host = pluginHost.load(std::memory_order_acquire))
            host->post({PlayerEventKind::kChat, std::string{player}, {}, {}, std::string{message}});
    };
    services.playerKick = [&pluginHost](std::string_view player, std::string_view reason) {
        if (auto* host = pluginHost.load(std::memory_order_acquire))
            host->post({PlayerEventKind::kKick, std::string{player}, {}, {}, std::string{reason}});
    };
    services.statusPing = [&pluginHost](std::string_view host, std::int32_t protocol) {
        if (auto* host_ = pluginHost.load(std::memory_order_acquire))
            host_->post({PlayerEventKind::kPing, {}, std::string{host}, {}, std::to_string(protocol)});
    };
    const auto loginGate = [&](std::string_view player, std::string_view uuid,
                               std::string_view address) -> std::optional<std::string> {
        (*players)->post([&players, &permissions, &serverList, player = std::string{player}, uuid = std::string{uuid},
                          address = std::string{address}] {
            (*players)->recordLogin(
                uuid, player, (*serverList)->rememberIp() ? std::optional<std::string_view>{address} : std::nullopt);
            permissions.onLogin(uuid, player);
        });
        if (auto turnedAway = access.check(player, uuid, address))
            return turnedAway;
        if (auto* host = pluginHost.load(std::memory_order_acquire))
            host->post({PlayerEventKind::kLogin, std::string{player}, {}, std::string{uuid}, {}});
        return std::nullopt;
    };
    services.playerLogin = loginGate;
    services.uiConfigs = [&ui] { return (*ui)->panels(); };
    services.placeholders = &(*serverList)->placeholders();
    services.loadingMessage = [&serverList]() -> std::optional<std::string> {
        if ((*serverList)->isLoading())
            return std::string{"<red>The proxy is loading, please wait</red>"};
        return std::nullopt;
    };
    services.playerEvents = [&pluginHost, &roster, &serverList](PlayerEvent event) {
        roster.apply(event);
        if (event.kind == PlayerEventKind::kJoin)
            (*serverList)->playerJoined(event.uuid, event.player);
        else if (event.kind == PlayerEventKind::kLeave)
            (*serverList)->playerLeft(event.uuid);
        if (auto* host = pluginHost.load(std::memory_order_acquire))
            host->post(std::move(event));
    };
    services.pluginMessage = [&pluginHost](std::string_view player, std::string_view channel,
                                           std::span<const std::uint8_t> data, bool fromBackend) {
        auto* host = pluginHost.load(std::memory_order_acquire);
        return host != nullptr && host->message(player, channel, data, fromBackend);
    };
    services.latency = [&pluginHost](std::string_view player, std::int32_t milliseconds) {
        if (auto* host = pluginHost.load(std::memory_order_acquire))
            host->post({PlayerEventKind::kLatency, std::string{player}, {}, {}, {}, {}, 0, milliseconds});
    };
    services.decide = [&pluginHost](DecisionRequest request) {
        auto* host = pluginHost.load(std::memory_order_acquire);
        return host != nullptr && host->decide(std::move(request));
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
    std::mutex queryMutex;
    std::unique_ptr<detail::QueryServer> query;
    proxy::config::QueryConfig activeQuery;
    const auto startQuery = [&](const proxy::config::QueryConfig& wanted) {
        query.reset();
        activeQuery = wanted;
        if (!wanted.enabled)
            return true;
        query = detail::QueryServer::start(
            wanted.port,
            [&runtime, &configuration] {
                detail::QueryServer::Info info;
                info.serverName = "Basalt Proxy";
                info.worldName = "world";
                info.version = std::string{buildInfo::kMinecraftVersion};
                info.hostIp = configuration->network.bind;
                info.serverPort = configuration->network.port;
                info.maxPlayers = static_cast<std::int32_t>(configuration->runtime.maxSessions);
                info.players = runtime->playerNames();
                return info;
            },
            logger);
        return query != nullptr;
    };
    std::mutex reloadMutex;
    const std::function<std::expected<std::string, std::string>()> reloadOutcome =
        [&]() -> std::expected<std::string, std::string> {
        const std::lock_guard reloadLock{reloadMutex};
        auto fresh = proxy::config::load(command.configDirectory);
        if (!fresh)
            return std::unexpected("Reload rejected, nothing changed: " + fresh.error());
        if (auto checked = proxy::config::loadServerList(command.configDirectory); !checked)
            return std::unexpected("Reload rejected, nothing changed: " + checked.error());
        if (auto checked = proxy::config::loadUi(command.configDirectory); !checked)
            return std::unexpected("Reload rejected, nothing changed: " + checked.error());
        auto freshGroups = proxy::config::loadGroups(command.configDirectory);
        if (!freshGroups)
            return std::unexpected("Reload rejected, nothing changed: " + freshGroups.error());
        auto freshCommands = proxy::config::loadCommands(command.configDirectory);
        if (!freshCommands)
            return std::unexpected("Reload rejected, nothing changed: " + freshCommands.error());
        auto freshMessages = proxy::config::loadMessages(command.configDirectory);
        if (!freshMessages)
            return std::unexpected("Reload rejected, nothing changed: " + freshMessages.error());
        if (auto listed = (*serverList)->reload(); !listed)
            return std::unexpected("Reload rejected, nothing changed: " + listed.error());
        if (auto drawn = (*ui)->reload(); !drawn)
            return std::unexpected("Reload rejected, nothing changed: " + drawn.error());
        const auto freshAccess = fresh->access;
        const auto freshQuery = fresh->query;
        const bool startupChanged = fresh->general.startupCommands != configuration->general.startupCommands;
        auto applied = runtime->reload(std::move(*fresh));
        if (!applied)
            return std::unexpected("Reload rejected, nothing changed: " + applied.error());
        permissions.reload(std::move(*freshGroups));
        dispatcher.configure(std::make_shared<const wire::config::CommandsConfig>(std::move(*freshCommands)));
        messages.reload(std::move(*freshMessages));
        enforceWhitelist.store(freshAccess.enforceWhitelist, std::memory_order_relaxed);
        if (freshAccess.whitelist != whitelistSetting) {
            whitelistSetting = freshAccess.whitelist;
            access.setWhitelist(freshAccess.whitelist);
        }
        std::string message = "Reloaded " + std::to_string(*applied) +
                              " backend(s), limits.toml, [general] and the access options of [network]; sessions in "
                              "progress keep their snapshot.";
        {
            const std::lock_guard lock{queryMutex};
            if (freshQuery != activeQuery) {
                message += startQuery(freshQuery) ? (freshQuery.enabled ? " Query restarted." : " Query stopped.")
                                                  : " Query could not bind its port and is off.";
            }
        }
        if (startupChanged)
            message += " startup-commands only run when the proxy starts.";
        if (auto* host = pluginHost.load(std::memory_order_acquire))
            message += " Reloaded " + std::to_string(host->reload()) + " plugin(s).";
        return message;
    };
    const std::function<std::string()> reloadConfiguration = [&]() -> std::string {
        auto outcome = reloadOutcome();
        return outcome ? std::move(*outcome) : std::move(outcome.error());
    };
    detail::CommandEnvironment environment;
    environment.players = players->get();
    environment.permissions = &permissions;
    environment.access = &access;
    environment.roster = &roster;
    environment.messages = &messages;
    environment.dispatcher = &dispatcher;
    environment.onlineMode = configuration->network.onlineMode;
    environment.servers = backendNames;
    environment.send = [&runtime, &bedrockPlayers](std::string_view player, std::string_view backend) {
        auto* bedrockSide = bedrockPlayers.load(std::memory_order_acquire);
        if (bedrockSide != nullptr && bedrockSide->connect(player, backend))
            return detail::SendResult::kAccepted;
        auto request = runtime->sendPlayer(player, backend);
        if (!request)
            return request.error() == BackendChangeError::kBusy ? detail::SendResult::kBusy
                                                                : detail::SendResult::kNotFound;
        for (int waited = 0; waited < 200; ++waited) {
            if (const auto result = request->result()) {
                switch (*result) {
                case BackendChangeResult::kAccepted:
                    return detail::SendResult::kAccepted;
                case BackendChangeResult::kPlayerNotFound:
                    return detail::SendResult::kNotFound;
                case BackendChangeResult::kUnavailable:
                    return detail::SendResult::kUnavailable;
                case BackendChangeResult::kStopped:
                    return detail::SendResult::kStopped;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }
        return detail::SendResult::kBusy;
    };
    environment.kick = kickPlayer;
    environment.reload = reloadOutcome;
    environment.status = [&runtime, &bedrockPlayers] {
        auto* side = bedrockPlayers.load(std::memory_order_acquire);
        return detail::statusText(*runtime, side != nullptr ? std::optional{side->playingCount()} : std::nullopt);
    };
    environment.stop = [&stopping] { stopping.request_stop(); };
    environment.plugins = [&pluginHost] {
        auto* host = pluginHost.load(std::memory_order_acquire);
        return host != nullptr ? host->names() : std::vector<std::string>{};
    };
    environment.enforceWhitelist = [&enforceWhitelist] { return enforceWhitelist.load(std::memory_order_relaxed); };
    detail::registerBuiltinCommands(environment);
    std::unique_ptr<proxy::plugins::PluginHost> plugins;
    if (configuration->plugins.enabled) {
        proxy::plugins::PluginSettings settings;
        settings.directory = configuration->plugins.directory;
        settings.database = configuration->plugins.database;
        settings.allow = configuration->plugins.allow;
        settings.limits.instructionsPerTurn = configuration->plugins.instructionsPerTurn;
        settings.limits.memoryBytes = configuration->plugins.memoryMib * 1024 * 1024;
        settings.queueSize = configuration->plugins.queueSize;
        settings.packets = packetRegistry;
        settings.packetTimeout = std::chrono::milliseconds{configuration->plugins.packetTimeoutMs};
        settings.packetMaxBytes = configuration->plugins.packetMaxKib * 1024;
#if defined(BASALT_HAS_SHIM)
        proxy::plugins::JavaRuntimeOptions javaOptions;
        javaOptions.pluginsDirectory = settings.directory;
        if (const char* runtime = std::getenv("BASALT_SHIM_DIR"))
            javaOptions.runtimeDirectory = runtime;
        settings.adapters.push_back([javaOptions](const proxy::plugins::PluginHooks& hooks) {
            return proxy::bungee::createBungeeAdapter(javaOptions, hooks);
        });
        settings.adapters.push_back([javaOptions](const proxy::plugins::PluginHooks& hooks) {
            return proxy::velocity::createVelocityAdapter(javaOptions, hooks);
        });
#endif
        proxy::plugins::PluginHooks hooks;
        hooks.packet = [&runtime](std::string_view player, bool serverbound, std::int32_t phase, std::int32_t id,
                                  std::span<const std::uint8_t> body) {
            return runtime->sendPacket(player, serverbound, phase, id, body);
        };
        hooks.servers = [&runtime] { return runtime->backendNames(); };
        hooks.info = [&configuration] {
            proxy::plugins::ProxyInfo info;
            info.bind = configuration->network.bind;
            info.port = configuration->network.port;
            info.onlineMode = configuration->network.onlineMode;
            info.preventClientProxyConnections = configuration->network.preventClientProxyConnections;
            info.compressionThreshold = configuration->network.compressionThreshold;
            info.connectTimeoutMs = configuration->runtime.connectTimeoutMs;
            info.readTimeoutMs = configuration->runtime.readTimeoutMs;
            info.maxSessions = configuration->runtime.maxSessions;
            info.defaultRoute = configuration->defaultRoute;
            for (const auto& route : configuration->forcedHosts)
                info.forcedHosts.emplace_back(route.host, route.backends);
            return info;
        };
        hooks.send = [&runtime, &bedrockPlayers](std::string_view player, std::string_view backend) {
            auto* bedrockSide = bedrockPlayers.load(std::memory_order_acquire);
            if (bedrockSide != nullptr && bedrockSide->connect(player, backend))
                return true;
            return runtime->sendPlayer(player, backend).has_value();
        };
        hooks.tell = tellPlayer;
        hooks.kick = kickPlayer;
        hooks.console = [&commandService, &consoleSource](std::string_view line) {
            return commandService.submit(consoleSource, std::string{line});
        };
        hooks.commands = &dispatcher;
        hooks.hasPermission = [&permissions](std::string_view player, std::string_view node) {
            return permissions.hasPlayer(player, node);
        };
        hooks.sendMessage = [&runtime](std::string_view player, std::string_view channel,
                                       std::span<const std::uint8_t> data, bool towardBackend) {
            return runtime->sendPluginMessage(player, channel, data, towardBackend);
        };
        hooks.serverAddress = [&runtime](std::string_view server) { return runtime->backendEndpoint(server); };
        hooks.tellComponent = [&runtime](std::string_view player, wire::text::Component component) {
            static_cast<void>(runtime->tellComponent(player, std::move(component)));
        };
        hooks.kickComponent = [&runtime](std::string_view player, wire::text::Component component) {
            static_cast<void>(runtime->kickComponent(player, std::move(component)));
        };
        hooks.resolveDecision = [&runtime](std::string_view player, std::uint64_t token, DecisionResult result) {
            runtime->resolveDecision(player, token, std::move(result));
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
#if !defined(_WIN32)
    const auto oldReload = std::signal(SIGHUP, reloadSignal);
#endif
    const auto fingerprint = [&command, &configuration] {
        std::map<std::string, std::pair<std::filesystem::file_time_type, std::uintmax_t>> seen;
        std::error_code error;
        const auto add = [&seen, &error](const std::filesystem::path& root, bool recursive) {
            if (!std::filesystem::is_directory(root, error))
                return;
            const auto record = [&seen, &error](const std::filesystem::directory_entry& entry) {
                if (entry.is_regular_file(error))
                    seen[entry.path().string()] = {entry.last_write_time(error), entry.file_size(error)};
            };
            if (recursive)
                for (const auto& entry : std::filesystem::recursive_directory_iterator{root, error})
                    record(entry);
            else
                for (const auto& entry : std::filesystem::directory_iterator{root, error})
                    record(entry);
        };
        add(command.configDirectory, false);
        if (configuration->plugins.enabled)
            add(configuration->plugins.directory, true);
        return seen;
    };
    std::jthread signals{[&](std::stop_token stop) {
        auto watched = fingerprint();
        auto pending = watched;
        bool changing = false;
        auto nextLook = std::chrono::steady_clock::now();
        while (!stop.stop_requested() && !signalStop.load(std::memory_order_relaxed)) {
            if (signalReload.exchange(false, std::memory_order_relaxed)) {
                logger.info([&] { return reloadConfiguration(); });
                watched = pending = fingerprint();
                changing = false;
            }
            if (configuration->reload.watchFiles && std::chrono::steady_clock::now() >= nextLook) {
                nextLook =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds{configuration->reload.intervalMs};
                auto current = fingerprint();
                if (changing && current == pending) {
                    logger.info([&] { return "Files changed. " + reloadConfiguration(); });
                    watched = pending = current;
                    changing = false;
                } else if (current != watched) {
                    changing = true;
                    pending = std::move(current);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        if (signalStop.load(std::memory_order_relaxed))
            stopping.request_stop();
    }};
    logger.info([&runtime] { return "Listening on port " + std::to_string(runtime->port()) + "."; });
    logger.warn([] {
        return std::string{"Proxy is under development; only verified protocol/backend combinations are supported."};
    });
    std::unique_ptr<detail::BedrockStatus> bedrock;
    if (configuration->bedrock.enabled) {
        std::shared_ptr<const detail::NativeForwarding> nativeForwarding;
        if (std::ranges::any_of(configuration->backends, [](const proxy::config::BackendConfig& backend) {
                return backend.forwarding == wire::auth::forwarding::ForwardingMode::Basalt;
            })) {
            auto signing = detail::loadOrCreateForwardingKey(configuration->forwarding.keyFile);
            if (!signing) {
                logger.error([&signing] { return signing.error(); });
                return 1;
            }
            nativeForwarding = std::make_shared<const detail::NativeForwarding>(detail::NativeForwarding{
                std::move(*signing), configuration->forwarding.proxyId, configuration->network.onlineMode,
                configuration->translation == proxy::config::TranslationMode::Auto, configuration->bedrock.delegate});
        }
        auto language = detail::loadBedrockLanguage(configuration->bedrock.language);
        if (!language) {
            logger.error([&language] { return language.error(); });
            return 1;
        }
        bedrock = std::make_unique<detail::BedrockStatus>(
            configuration->bedrock, [&serverList] { return (*serverList)->bedrockMotd(); },
            [&runtime] { return runtime->sessionCount(); }, configuration->runtime.maxSessions,
            [&configuration]() -> std::optional<proxy::config::BackendConfig> {
                const auto routes = detail::BackendRoutes::create(*configuration);
                if (!routes)
                    return std::nullopt;
                const auto defaults = routes->select("bedrock.invalid");
                if (defaults.empty())
                    return std::nullopt;
                return configuration->backends[defaults.front()];
            },
            [&pluginHost, &roster](PlayerEvent event) {
                roster.apply(event);
                if (auto* host = pluginHost.load(std::memory_order_acquire))
                    host->post(std::move(event));
            },
            *language,
            [&configuration](std::string_view name) -> std::optional<proxy::config::BackendConfig> {
                const auto found =
                    std::ranges::find(configuration->backends, name, &proxy::config::BackendConfig::name);
                if (found == configuration->backends.end())
                    return std::nullopt;
                return *found;
            },
            nativeForwarding, configuration->network.onlineMode, configuration->admission,
            detail::BedrockSession::Commands{playerCommand,
                                             playerCommandSpecs,
                                             {},
                                             [&configuration] {
                                                 std::vector<std::string> names;
                                                 for (const auto& backend : configuration->backends)
                                                     names.push_back(backend.name);
                                                 return names;
                                             },
                                             loginGate});
        if (!bedrock->start()) {
            logger.error([&configuration] {
                return "Could not start Bedrock on port " + std::to_string(configuration->bedrock.port) +
                       "; see the messages above.";
            });
            return 1;
        }
        bedrockPlayers.store(bedrock.get(), std::memory_order_release);
        logger.info([&bedrock] {
            std::string line = "Bedrock over";
            if (bedrock->rakNetOn())
                line += " RakNet on UDP port " + std::to_string(bedrock->port());
            if (bedrock->netherNetOn())
                line += std::string{bedrock->rakNetOn() ? " and" : ""} + " NetherNet on TCP port " +
                        std::to_string(bedrock->port()) + " and UDP port " +
                        std::to_string(bedrock->netherNetIcePort());
            return line + ".";
        });
    }
    {
        const std::lock_guard lock{queryMutex};
        if (!startQuery(configuration->query)) {
            logger.error([] { return std::string{"Could not bind the query port."}; });
            return 1;
        }
    }
    std::unique_ptr<proxy::remote::RemoteApi> remoteApi;
    if (remoteConfiguration->api.enabled) {
        proxy::remote::RemoteHooks hooks;
        hooks.runtime = &*runtime;
        hooks.command = [&dispatcher](std::string_view line) {
            proxy::remote::CommandReply reply;
            CommandSource source{"API", true, [&reply](std::string_view miniMessage) {
                                     reply.lines.push_back(detail::Messages::plain(miniMessage));
                                 }};
            const CommandResult result = dispatcher.execute(source, line);
            reply.outcome = result.outcome;
            return reply;
        };
        hooks.playerOnline = [&roster](std::string_view player) { return roster.find(player).has_value(); };
        if (configuration->bedrock.enabled) {
            hooks.bedrockPlaying = [&bedrockPlayers]() -> std::size_t {
                auto* side = bedrockPlayers.load(std::memory_order_acquire);
                return side != nullptr ? side->playingCount() : 0;
            };
        }
        hooks.loadedPlugins = [&plugins]() -> std::size_t { return plugins ? plugins->loaded() : 0; };
        auto started = proxy::remote::RemoteApi::start(*remoteConfiguration, std::move(hooks), ".basalt/api.token");
        if (!started) {
            logger.error([&started] { return started.error(); });
            return 1;
        }
        remoteApi = std::move(*started);
        logger.info([&] {
            return "Remote API on " + remoteConfiguration->api.bind + ":" + std::to_string(remoteApi->port()) + ".";
        });
        if (remoteConfiguration->api.bind != "127.0.0.1" && remoteConfiguration->api.bind != "::1")
            logger.warn([] {
                return std::string{
                    "The remote API speaks plain HTTP on a non-loopback address; put TLS in front of it."};
            });
    }
    std::jthread updateChecker;
    if (configuration->runtime.updateChecker && updateChecker::parseVersion(buildInfo::kVersion).has_value()) {
        updateChecker = std::jthread{
            [&logger](std::stop_token stop) { updateChecker::checkAndAnnounce(buildInfo::kVersion, logger, stop); }};
    }
    std::jthread console{[&](std::stop_token stop) {
        for (const auto& line : configuration->general.startupCommands) {
            consoleLogger.info([&] { return "Startup command '" + line + "'"; });
            commandService.run(consoleSource, line);
            if (stopping.stop_requested())
                return;
        }
        detail::runConsole(commandService, dispatcher, stop);
    }};
    (*serverList)->setLoading(false);
    const auto result = runtime->run(stopping.get_token());
    remoteApi.reset();
    if (updateChecker.joinable()) {
        updateChecker.request_stop();
        updateChecker.join();
    }
    console.request_stop();
    console.join();
    bedrockPlayers.store(nullptr, std::memory_order_release);
    bedrock.reset();
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
