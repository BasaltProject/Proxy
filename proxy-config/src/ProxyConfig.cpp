#include "ConfigReader.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/io/ConfigFiles.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

namespace basalt::proxy::config {

std::expected<ProxyConfig, std::string> fromToml(const detail::TomlTable& proxy, const detail::TomlTable& backends,
                                                 std::string_view directory) {
    ProxyConfig result;
    detail::Reader reader{std::string{directory} + "/proxy.toml", {}};
    reader.keys(&proxy, {"network", "runtime", "admission", "forwarding", "logging", "translation"}, "");
    const auto* network = reader.child(proxy, "network");
    reader.keys(network,
                {"bind", "port", "dual-stack", "online-mode", "accept-transfers", "prevent-client-proxy-connections",
                 "compression-threshold"},
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
    if (!wire::io::TcpAddress::literal(result.network.bind, result.network.port))
        reader.fail("network.bind", "must be a literal IPv4 or IPv6 address");
    if (result.network.dualStack && result.network.bind.find(':') == std::string::npos)
        reader.fail("network.dual-stack", "requires an IPv6 bind address");

    const auto* runtime = reader.child(proxy, "runtime");
    reader.keys(runtime,
                {"io-loops", "workers", "max-sessions", "max-pending-work", "max-buffered-bytes",
                 "session-buffer-bytes", "high-water-bytes", "low-water-bytes", "handshake-timeout-ms",
                 "login-timeout-ms", "connect-timeout-ms", "read-timeout-ms", "write-timeout-ms"},
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
    if (result.runtime.lowWaterBytes >= result.runtime.highWaterBytes ||
        result.runtime.highWaterBytes >= result.runtime.sessionBufferBytes)
        reader.fail("runtime", "requires low-water-bytes < high-water-bytes < session-buffer-bytes");
    if (result.runtime.sessionBufferBytes > result.runtime.maxBufferedBytes)
        reader.fail("runtime.session-buffer-bytes", "exceeds the global buffer budget");

    const auto* admission = reader.child(proxy, "admission");
    reader.keys(admission,
                {"global-rate", "global-burst", "origin-rate", "origin-burst", "max-origins", "origin-expiry-ms"},
                "admission.");
    reader.integer(admission, "global-rate", result.admission.globalRate, 1, 1000000, "admission.");
    reader.integer(admission, "global-burst", result.admission.globalBurst, 1, 1000000, "admission.");
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

    if (reader.error)
        return std::unexpected(*reader.error);
    auto parsedBackends = detail::readBackends(result, backends, std::string{directory} + "/backends.toml");
    if (!parsedBackends)
        return std::unexpected(parsedBackends.error());
    return result;
}

std::expected<ProxyConfig, std::string> load(std::string_view directory) {
    auto proxy = wire::io::configFiles::loadToml(directory, "proxy.toml", defaultProxyToml());
    if (!proxy)
        return std::unexpected(proxy.error());
    auto backends = wire::io::configFiles::loadToml(directory, "backends.toml", defaultBackendsToml());
    if (!backends)
        return std::unexpected(backends.error());
    return fromToml(proxy->values, backends->values, directory);
}

} // namespace basalt::proxy::config
