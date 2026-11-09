#include "MainPlugins.hpp"

#include "BedrockStatus.hpp"

#include <basalt/proxy/bungee/BungeeAdapter.hpp>
#include <basalt/proxy/velocity/VelocityAdapter.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/io/ByteBuffer.hpp>
#include <basalt/wire/protocol/PacketNames.hpp>
#include <basalt/wire/protocol/play/ChatPackets.hpp>
#include <basalt/wire/protocol/play/CommandPackets.hpp>

#include <chrono>
#include <cstdlib>

using namespace basalt;

namespace basalt::proxy::runtime::detail {

PluginSetup makePluginSetup(const std::expected<config::ProxyConfig, std::string>& configuration,
                            std::expected<ProxyRuntime, std::string>& runtime,
                            std::atomic<BedrockStatus*>& bedrockPlayers,
                            const decltype(plugins::PluginHooks::tell)& tellPlayer,
                            const decltype(plugins::PluginHooks::kick)& kickPlayer, CommandService& commandService,
                            const CommandSource& consoleSource, CommandDispatcher& dispatcher,
                            PermissionService& permissions,
                            const std::shared_ptr<wire::plugins::PacketRegistry>& packetRegistry) {
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
    if (const char* shimDirectory = std::getenv("BASALT_SHIM_DIR"))
        javaOptions.runtimeDirectory = shimDirectory;
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
    hooks.spoofChat = [&runtime](std::string_view player, std::string_view text) {
        namespace play = wire::protocol::play;
        wire::io::ByteBuffer body;
        std::int32_t id = play::ChatMessagePacket::kId;
        if (!text.empty() && text.front() == '/') {
            id = play::ChatCommandPacket::kId;
            play::ChatCommandPacket{std::string{text.substr(1)}}.write(body);
        } else {
            play::ChatMessagePacket packet;
            packet.message = std::string{text.substr(0, play::ChatMessagePacket::kMaxLength)};
            packet.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
            packet.write(body);
        }
        return runtime->sendPacket(player, true, static_cast<std::int32_t>(wire::protocol::packetNames::Phase::kPlay),
                                   id, body.readable());
    };
    hooks.permissionOverride = [&permissions](std::string_view player, std::string_view node) {
        const auto value = permissions.overrideOf(player, node);
        return value ? (*value ? 1 : 0) : -1;
    };
    hooks.permissionList = [&permissions](std::string_view player) { return permissions.effective(player); };
    hooks.groupOf = [&permissions](std::string_view player) { return permissions.groupOf(player); };
    hooks.setGroup = [&permissions](std::string_view player, std::string_view group) {
        if (group.empty())
            return permissions.setGroup(player, std::nullopt);
        return permissions.groupExists(group) && permissions.setGroup(player, std::string{group});
    };
    hooks.setPermission = [&permissions](std::string_view player, std::string_view node, int value) {
        permissions.setOverride(player, node, value < 0 ? std::nullopt : std::optional<bool>{value != 0});
    };
    hooks.sendMessage = [&runtime](std::string_view player, std::string_view channel,
                                   std::span<const std::uint8_t> data, bool towardBackend) {
        return runtime->sendPluginMessage(player, channel, data, towardBackend);
    };
    hooks.serverAddress = [&runtime](std::string_view server) { return runtime->backendEndpoint(server); };
    hooks.tellComponent = [&runtime](std::string_view player, wire::text::Component component) {
        static_cast<void>(runtime->tellComponent(player, std::move(component)));
    };
    hooks.hud = [&runtime](std::string_view player, PlayerHud action) {
        return runtime->sendHud(player, std::move(action));
    };
    hooks.requestCookie = [&runtime](std::string_view player, std::string_view key,
                                     std::function<void(std::optional<std::vector<std::uint8_t>>)> reply) {
        return runtime->requestCookie(player, key, std::move(reply));
    };
    hooks.kickComponent = [&runtime](std::string_view player, wire::text::Component component) {
        static_cast<void>(runtime->kickComponent(player, std::move(component)));
    };
    hooks.resolveDecision = [&runtime](std::string_view player, std::uint64_t token, DecisionResult result) {
        runtime->resolveDecision(player, token, std::move(result));
    };
    hooks.antibotVerify = [&runtime](std::string_view player) { runtime->antibotVerify(player); };
    hooks.antibotUnverify = [&runtime](std::string_view player) { runtime->antibotUnverify(player); };
    hooks.antibotVerified = [&runtime](std::string_view player) { return runtime->antibotVerified(player); };
    hooks.antibotAttack = [&runtime] { return runtime->antibotAttack(); };
    hooks.antibotBan = [&runtime](std::string_view address, bool ban) {
        const std::array<std::string_view, 2> words{ban ? "ban" : "unban", address};
        return runtime->antibotCommand(words).starts_with("Origin");
    };
    hooks.log = [](std::string_view plugin, std::string_view line) {
        wire::common::Logger{"Plugin " + std::string{plugin}}.info([&line] { return std::string{line}; });
    };
    return PluginSetup{std::move(settings), std::move(hooks)};
}

} // namespace basalt::proxy::runtime::detail
