#pragma once

#include <basalt/wire/auth/forwarding/ProxyForwarding.hpp>
#include <basalt/wire/common/logging/LogLevel.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::wire::common::toml {
class TomlTable;
}

namespace basalt::proxy::config {

enum class TranslationMode { Auto, Proxy, Off };
enum class ColorMode { Auto, Always, Never };

struct NetworkConfig {
    std::string bind = "0.0.0.0";
    std::uint16_t port = 25565;
    bool dualStack = false;
    bool onlineMode = true;
    bool acceptTransfers = false;
    bool preventClientProxyConnections = false;
    std::int32_t compressionThreshold = 256;

    friend bool operator==(const NetworkConfig&, const NetworkConfig&) = default;
};

struct RuntimeConfig {
    std::size_t ioLoops = 2;
    std::size_t workers = 4;
    std::size_t maxSessions = 4096;
    std::size_t maxPendingWork = 256;
    std::size_t maxBufferedBytes = 256 * 1024 * 1024;
    std::size_t sessionBufferBytes = 16 * 1024 * 1024;
    std::size_t highWaterBytes = 4 * 1024 * 1024;
    std::size_t lowWaterBytes = 2 * 1024 * 1024;
    std::uint32_t handshakeTimeoutMs = 10000;
    std::uint32_t loginTimeoutMs = 30000;
    std::uint32_t connectTimeoutMs = 10000;
    std::uint32_t readTimeoutMs = 30000;
    std::uint32_t writeTimeoutMs = 10000;

    friend bool operator==(const RuntimeConfig&, const RuntimeConfig&) = default;
};

struct AdmissionConfig {
    std::uint32_t globalRate = 200;
    std::uint32_t globalBurst = 400;
    std::uint32_t originRate = 10;
    std::uint32_t originBurst = 20;
    std::size_t maxOrigins = 65536;
    std::uint32_t originExpiryMs = 60000;

    friend bool operator==(const AdmissionConfig&, const AdmissionConfig&) = default;
};

struct ForwardingConfig {
    std::string proxyId = "proxy";
    std::string keyFile = ".basalt/forwarding.key";

    friend bool operator==(const ForwardingConfig&, const ForwardingConfig&) = default;
};

struct LoggingConfig {
    wire::common::LogLevel level = wire::common::LogLevel::Info;
    bool file = true;
    std::int32_t keepDays = 14;
    ColorMode color = ColorMode::Auto;

    friend bool operator==(const LoggingConfig&, const LoggingConfig&) = default;
};

struct PluginsConfig {
    bool enabled = false;
    std::string directory = "plugins";
    std::string database = ".basalt/proxy.db";
    std::int64_t instructionsPerTurn = 250000;
    std::size_t memoryMib = 32;
    std::size_t queueSize = 256;
    /// Plugin id to the permissions the operator grants it; a manifest asking for more is not loaded.
    std::map<std::string, std::vector<std::string>> allow;
    /// Players who may run a plugin command that asks for a permission.
    std::vector<std::string> operators;

    friend bool operator==(const PluginsConfig&, const PluginsConfig&) = default;
};

struct BedrockConfig {
    bool enabled = false;
    std::uint16_t port = 19132;
    std::string motd = "Basalt Proxy";

    friend bool operator==(const BedrockConfig&, const BedrockConfig&) = default;
};

struct ReloadConfig {
    bool watchFiles = false;
    std::uint32_t intervalMs = 2000;

    friend bool operator==(const ReloadConfig&, const ReloadConfig&) = default;
};

struct BackendConfig {
    bool bungeeMessages = false;
    std::string name;
    std::string host;
    std::uint16_t port = 25566;
    /// The backend's protocol, or kClientProtocol when it speaks whatever version the player connects with.
    std::int32_t protocol = 777;
    static constexpr std::int32_t kClientProtocol = 0;
    wire::auth::forwarding::ForwardingMode forwarding = wire::auth::forwarding::ForwardingMode::None;
    std::string secret;
    std::string propertyName{wire::auth::forwarding::ProxyForwarding::kSafeNetProperty};
};

struct HostRoute {
    std::string host;
    std::vector<std::string> backends;
};

struct ProxyConfig {
    NetworkConfig network;
    RuntimeConfig runtime;
    AdmissionConfig admission;
    ForwardingConfig forwarding;
    LoggingConfig logging;
    PluginsConfig plugins;
    BedrockConfig bedrock;
    ReloadConfig reload;
    TranslationMode translation = TranslationMode::Auto;
    std::vector<BackendConfig> backends;
    std::vector<std::string> defaultRoute;
    std::vector<HostRoute> forcedHosts;
};

[[nodiscard]] std::string_view defaultProxyToml() noexcept;
[[nodiscard]] std::string_view defaultBackendsToml() noexcept;
[[nodiscard]] std::string_view defaultPluginsToml() noexcept;
[[nodiscard]] std::expected<ProxyConfig, std::string> fromToml(const wire::common::toml::TomlTable& proxy,
                                                               const wire::common::toml::TomlTable& backends,
                                                               std::string_view directory,
                                                               const wire::common::toml::TomlTable* plugins = nullptr);
[[nodiscard]] std::expected<ProxyConfig, std::string> load(std::string_view directory);

} // namespace basalt::proxy::config
