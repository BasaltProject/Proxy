#include "ConfigReader.hpp"

#include <basalt/proxy/config/AddressRange.hpp>
#include <basalt/proxy/config/ConfigDefaults.hpp>
#include <basalt/proxy/config/PatternSafety.hpp>
#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

#include <algorithm>
#include <cctype>
#include <regex>

namespace basalt::proxy::config {

std::expected<ProxyConfig, std::string> fromToml(const detail::TomlTable& proxy, const detail::TomlTable& servers,
                                                 std::string_view directory, const detail::TomlTable* pluginsTable,
                                                 const detail::TomlTable* limitsTable,
                                                 const detail::TomlTable* antibotTable) {
    ProxyConfig result;
    detail::Reader reader{std::string{directory} + "/proxy.toml", {}};
    reader.keys(&proxy,
                {"network", "general", "query", "runtime", "admission", "forwarding", "logging", "bedrock", "reload",
                 "translation", "access"},
                "");
    const auto* network = reader.child(proxy, "network");
    reader.keys(network,
                {"bind", "port", "dual-stack", "online-mode", "accept-transfers", "prevent-client-proxy-connections",
                 "compression-threshold", "proxy-protocol", "proxy-protocol-trusted", "connection-throttle-ms",
                 "max-connections-per-address", "max-joins-per-tick"},
                "network.");
    reader.scalar(network, "bind", result.network.bind, "network.");
    reader.integer(network, "port", result.network.port, 1, 65535, "network.");
    reader.scalar(network, "dual-stack", result.network.dualStack, "network.");
    reader.scalar(network, "online-mode", result.network.onlineMode, "network.");
    reader.scalar(network, "accept-transfers", result.network.acceptTransfers, "network.");
    reader.scalar(network, "prevent-client-proxy-connections", result.network.preventClientProxyConnections,
                  "network.");
    reader.integer(network, "compression-threshold", result.network.compressionThreshold, -1, 8 * 1024 * 1024,
                   "network.");
    reader.scalar(network, "proxy-protocol", result.network.proxyProtocol, "network.");
    reader.stringList(network, "proxy-protocol-trusted", result.network.proxyProtocolTrusted, "network.");
    reader.integer(network, "connection-throttle-ms", result.network.connectionThrottleMs, 0, 3600000, "network.");
    reader.integer(network, "max-connections-per-address", result.network.maxConnectionsPerAddress, 0, 65536,
                   "network.");
    reader.integer(network, "max-joins-per-tick", result.network.maxJoinsPerTick, 0, 65536, "network.");
    for (const auto& entry : result.network.proxyProtocolTrusted)
        if (!AddressRange::parse(entry))
            reader.fail("network.proxy-protocol-trusted", "entry '" + entry + "' is not an IP address or CIDR range");
    if (result.network.proxyProtocol && result.network.proxyProtocolTrusted.empty())
        reader.fail("network.proxy-protocol-trusted", "must list the balancers allowed to send a PROXY header");
    if (!wire::io::TcpAddress::literal(result.network.bind, result.network.port))
        reader.fail("network.bind", "must be a literal IPv4 or IPv6 address");
    if (result.network.dualStack && result.network.bind.find(':') == std::string::npos)
        reader.fail("network.dual-stack", "requires an IPv6 bind address");

    const auto* general = reader.child(proxy, "general");
    reader.keys(general, {"brand", "startup-commands", "username-pattern", "enforce-secure-profile"}, "general.");
    reader.scalar(general, "brand", result.general.brand, "general.");
    reader.stringList(general, "startup-commands", result.general.startupCommands, "general.");
    reader.scalar(general, "username-pattern", result.general.usernamePattern, "general.");
    reader.scalar(general, "enforce-secure-profile", result.general.enforceSecureProfile, "general.");
    if (result.general.brand.size() > 64)
        reader.fail("general.brand", "must be at most 64 bytes");
    if (const auto reason = unsafePatternReason(result.general.usernamePattern)) {
        reader.fail("general.username-pattern", *reason);
    } else if (!result.general.usernamePattern.empty()) {
        try {
            static_cast<void>(std::regex{result.general.usernamePattern});
        } catch (const std::regex_error&) {
            reader.fail("general.username-pattern", "is not a valid regular expression");
        }
    }

    const auto* query = reader.child(proxy, "query");
    reader.keys(query, {"enabled", "port"}, "query.");
    reader.scalar(query, "enabled", result.query.enabled, "query.");
    reader.integer(query, "port", result.query.port, 1, 65535, "query.");

    const auto* runtime = reader.child(proxy, "runtime");
    reader.keys(runtime,
                {"io-loops", "workers", "max-sessions", "max-pending-work", "max-buffered-bytes",
                 "session-buffer-bytes", "high-water-bytes", "low-water-bytes", "handshake-timeout-ms",
                 "login-timeout-ms", "connect-timeout-ms", "read-timeout-ms", "write-timeout-ms", "update-checker"},
                "runtime.");
    reader.integer(runtime, "io-loops", result.runtime.ioLoops, 1, 64, "runtime.");
    reader.integer(runtime, "workers", result.runtime.workers, 1, 64, "runtime.");
    reader.integer(runtime, "max-sessions", result.runtime.maxSessions, 1, 65536, "runtime.");
    reader.integer(runtime, "max-pending-work", result.runtime.maxPendingWork, 1, 65536, "runtime.");
    constexpr std::int64_t kMaxBudget = 16LL * 1024 * 1024 * 1024;
    reader.integer(runtime, "max-buffered-bytes", result.runtime.maxBufferedBytes, 4096, kMaxBudget, "runtime.");
    reader.integer(runtime, "session-buffer-bytes", result.runtime.sessionBufferBytes, 4096, kMaxBudget, "runtime.");
    reader.integer(runtime, "high-water-bytes", result.runtime.highWaterBytes, 1, kMaxBudget, "runtime.");
    reader.integer(runtime, "low-water-bytes", result.runtime.lowWaterBytes, 0, kMaxBudget, "runtime.");
    reader.integer(runtime, "handshake-timeout-ms", result.runtime.handshakeTimeoutMs, 1, 300000, "runtime.");
    reader.integer(runtime, "login-timeout-ms", result.runtime.loginTimeoutMs, 1, 300000, "runtime.");
    reader.integer(runtime, "connect-timeout-ms", result.runtime.connectTimeoutMs, 1, 300000, "runtime.");
    reader.integer(runtime, "read-timeout-ms", result.runtime.readTimeoutMs, 1, 300000, "runtime.");
    reader.integer(runtime, "write-timeout-ms", result.runtime.writeTimeoutMs, 1, 300000, "runtime.");
    reader.scalar(runtime, "update-checker", result.runtime.updateChecker, "runtime.");
    if (result.runtime.lowWaterBytes >= result.runtime.highWaterBytes ||
        result.runtime.highWaterBytes >= result.runtime.sessionBufferBytes)
        reader.fail("runtime", "requires low-water-bytes < high-water-bytes < session-buffer-bytes");
    if (result.runtime.sessionBufferBytes > result.runtime.maxBufferedBytes)
        reader.fail("runtime.session-buffer-bytes", "exceeds the global buffer budget");

    const auto* admission = reader.child(proxy, "admission");
    reader.keys(admission,
                {"global-rate", "global-burst", "origin-rate", "origin-burst", "max-origins", "origin-expiry-ms",
                 "status-rate", "status-burst"},
                "admission.");
    reader.integer(admission, "global-rate", result.admission.globalRate, 1, 1000000, "admission.");
    reader.integer(admission, "global-burst", result.admission.globalBurst, 1, 1000000, "admission.");
    reader.integer(admission, "status-rate", result.admission.statusRate, 1, 1000000, "admission.");
    reader.integer(admission, "status-burst", result.admission.statusBurst, 1, 1000000, "admission.");
    reader.integer(admission, "origin-rate", result.admission.originRate, 1, 1000000, "admission.");
    reader.integer(admission, "origin-burst", result.admission.originBurst, 1, 1000000, "admission.");
    reader.integer(admission, "max-origins", result.admission.maxOrigins, 1, 1000000, "admission.");
    reader.integer(admission, "origin-expiry-ms", result.admission.originExpiryMs, 1000, 3600000, "admission.");

    const auto* forwarding = reader.child(proxy, "forwarding");
    reader.keys(forwarding, {"proxy-id", "key-file"}, "forwarding.");
    reader.scalar(forwarding, "proxy-id", result.forwarding.proxyId, "forwarding.");
    reader.scalar(forwarding, "key-file", result.forwarding.keyFile, "forwarding.");
    if (result.forwarding.proxyId.empty() || result.forwarding.proxyId.size() > 64)
        reader.fail("forwarding.proxy-id", "must contain 1..64 bytes");
    if (result.forwarding.keyFile.empty() || result.forwarding.keyFile.size() > 4096)
        reader.fail("forwarding.key-file", "must be a path of 1..4096 bytes");

    const auto* logging = reader.child(proxy, "logging");
    reader.keys(logging, {"level", "file", "keep-days", "color"}, "logging.");
    std::string level = "info";
    std::string color = "auto";
    reader.scalar(logging, "level", level, "logging.");
    reader.scalar(logging, "file", result.logging.file, "logging.");
    reader.integer(logging, "keep-days", result.logging.keepDays, 0, 3650, "logging.");
    reader.scalar(logging, "color", color, "logging.");
    if (level == "trace")
        result.logging.level = wire::common::LogLevel::Trace;
    else if (level == "debug")
        result.logging.level = wire::common::LogLevel::Debug;
    else if (level == "info")
        result.logging.level = wire::common::LogLevel::Info;
    else if (level == "warn")
        result.logging.level = wire::common::LogLevel::Warn;
    else if (level == "error")
        result.logging.level = wire::common::LogLevel::Error;
    else
        reader.fail("logging.level", "must be trace, debug, info, warn or error");
    if (color == "auto")
        result.logging.color = ColorMode::Auto;
    else if (color == "always")
        result.logging.color = ColorMode::Always;
    else if (color == "never")
        result.logging.color = ColorMode::Never;
    else
        reader.fail("logging.color", "must be auto, always or never");

    const auto* bedrock = reader.child(proxy, "bedrock");
    reader.keys(bedrock,
                {"enabled", "port", "transport", "nethernet-port", "advertise-address", "language", "delegate"},
                "bedrock.");
    reader.scalar(bedrock, "enabled", result.bedrock.enabled, "bedrock.");
    reader.integer(bedrock, "port", result.bedrock.port, 1, 65535, "bedrock.");
    std::string transport = "both";
    reader.scalar(bedrock, "transport", transport, "bedrock.");
    if (transport == "both")
        result.bedrock.transport = BedrockTransport::kBoth;
    else if (transport == "nethernet")
        result.bedrock.transport = BedrockTransport::kNetherNet;
    else if (transport == "raknet")
        result.bedrock.transport = BedrockTransport::kRakNet;
    else
        reader.fail("bedrock.transport", "must be both, nethernet or raknet");
    reader.integer(bedrock, "nethernet-port", result.bedrock.nethernetPort, 0, 65535, "bedrock.");
    reader.scalar(bedrock, "advertise-address", result.bedrock.advertiseAddress, "bedrock.");
    reader.scalar(bedrock, "language", result.bedrock.language, "bedrock.");
    reader.scalar(bedrock, "delegate", result.bedrock.delegate, "bedrock.");

    const auto* reload = reader.child(proxy, "reload");
    reader.keys(reload, {"watch-files", "interval-ms"}, "reload.");
    reader.scalar(reload, "watch-files", result.reload.watchFiles, "reload.");
    reader.integer(reload, "interval-ms", result.reload.intervalMs, 250, 600000, "reload.");

    const auto* access = reader.child(proxy, "access");
    reader.keys(access, {"whitelist", "enforce-whitelist"}, "access.");
    reader.scalar(access, "whitelist", result.access.whitelist, "access.");
    reader.scalar(access, "enforce-whitelist", result.access.enforceWhitelist, "access.");

    const auto* translation = reader.child(proxy, "translation");
    reader.keys(translation, {"mode"}, "translation.");
    std::string mode = "auto";
    reader.scalar(translation, "mode", mode, "translation.");
    if (mode == "auto")
        result.translation = TranslationMode::Auto;
    else if (mode == "proxy")
        result.translation = TranslationMode::Proxy;
    else if (mode == "off")
        result.translation = TranslationMode::Off;
    else
        reader.fail("translation.mode", "must be auto, proxy or off");

    if (pluginsTable) {
        detail::Reader plugins{std::string{directory} + "/plugins.toml", {}};
        plugins.keys(pluginsTable,
                     {"enabled", "directory", "database", "instructions-per-turn", "memory-mib", "queue-size",
                      "packet-max-kib", "packet-timeout-ms", "allow"},
                     "");
        plugins.scalar(pluginsTable, "enabled", result.plugins.enabled, "");
        plugins.scalar(pluginsTable, "directory", result.plugins.directory, "");
        plugins.scalar(pluginsTable, "database", result.plugins.database, "");
        plugins.integer(pluginsTable, "instructions-per-turn", result.plugins.instructionsPerTurn, 1000, 1000000000,
                        "");
        plugins.integer(pluginsTable, "memory-mib", result.plugins.memoryMib, 1, 1024, "");
        plugins.integer(pluginsTable, "queue-size", result.plugins.queueSize, 1, 65536, "");
        plugins.integer(pluginsTable, "packet-max-kib", result.plugins.packetMaxKib, 1, 2048, "");
        plugins.integer(pluginsTable, "packet-timeout-ms", result.plugins.packetTimeoutMs, 1, 1000, "");
        if (const auto* allow = plugins.child(*pluginsTable, "allow")) {
            for (const auto& id : allow->keys()) {
                auto names = plugins.strings(allow->find(id), "allow." + id);
                for (const auto& name : names)
                    if (name != "commands" && name != "network" && name != "packets")
                        plugins.fail("allow." + id, "grants unknown permission '" + name + "'");
                result.plugins.allow.emplace(id, std::move(names));
            }
        }
        if (result.plugins.database.empty() || result.plugins.database.size() > 4096)
            plugins.fail("database", "must be a path of 1..4096 bytes");
        if (result.plugins.directory.empty() || result.plugins.directory.size() > 4096)
            plugins.fail("directory", "must be a path of 1..4096 bytes");
        if (plugins.error)
            return std::unexpected(*plugins.error);
    }
    if (limitsTable) {
        detail::Reader limits{std::string{directory} + "/limits.toml", {}};
        limits.keys(limitsTable, {"packets", "spam"}, "");
        const auto* packets = limits.child(*limitsTable, "packets");
        limits.keys(packets, {"rate-limit", "drop", "kick"}, "packets.");
        limits.integer(packets, "rate-limit", result.limits.packetRateLimit, 0, 1 << 24, "packets.");
        for (const auto action : {PacketRule::Action::kDrop, PacketRule::Action::kKick}) {
            const std::string section = action == PacketRule::Action::kDrop ? "drop" : "kick";
            const auto* rules = packets ? limits.child(*packets, section) : nullptr;
            if (!rules)
                continue;
            for (const auto& name : rules->keys()) {
                const auto address = wire::protocol::packetNames::resolve(name);
                const auto limit = rules->integer(name);
                if (!address || !address->serverbound)
                    limits.fail("packets." + section + "." + name, "is not a packet a client sends");
                else if (!limit || *limit < 1 || *limit > (1 << 24))
                    limits.fail("packets." + section + "." + name, "must be a number of packets per second above 0");
                else
                    result.limits.packetRules.push_back(
                        PacketRule{name, *address, static_cast<std::int32_t>(*limit), action});
            }
        }
        const auto* spam = limits.child(*limitsTable, "spam");
        limits.keys(spam, {"chat-threshold-seconds", "command-threshold-seconds", "excluded-commands"}, "spam.");
        limits.integer(spam, "chat-threshold-seconds", result.limits.chatSpamThresholdSeconds, 0, 3600, "spam.");
        limits.integer(spam, "command-threshold-seconds", result.limits.commandSpamThresholdSeconds, 0, 3600, "spam.");
        limits.stringList(spam, "excluded-commands", result.limits.spamExcludedCommands, "spam.");
        for (auto& name : result.limits.spamExcludedCommands) {
            std::erase(name, '/');
            std::ranges::transform(name, name.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        if (limits.error)
            return std::unexpected(*limits.error);
    }
    if (antibotTable) {
        auto antibot = readAntibot(*antibotTable, directory);
        if (!antibot)
            return std::unexpected(antibot.error());
        result.antibot = std::move(*antibot);
    }
    if (reader.error)
        return std::unexpected(*reader.error);
    auto parsedServers = detail::readServers(result, servers, std::string{directory} + "/servers.toml");
    if (!parsedServers)
        return std::unexpected(parsedServers.error());
    return result;
}

std::expected<ProxyConfig, std::string> load(std::string_view directory) {
    const auto files = configDirectory(directory);
    auto proxy = files.load("proxy.toml", kProxyDefaultToml);
    if (!proxy)
        return std::unexpected(proxy.error());
    auto servers = files.load("servers.toml", kServersDefaultToml);
    if (!servers)
        return std::unexpected(servers.error());
    auto plugins = files.load("plugins.toml", kPluginsDefaultToml);
    if (!plugins)
        return std::unexpected(plugins.error());
    auto limits = files.load("limits.toml", kLimitsDefaultToml);
    if (!limits)
        return std::unexpected(limits.error());
    auto antibot = files.load("antibot.toml", kAntibotDefaultToml);
    if (!antibot)
        return std::unexpected(antibot.error());
    return fromToml(*proxy, *servers, directory, &*plugins, &*limits, &*antibot);
}

} // namespace basalt::proxy::config
