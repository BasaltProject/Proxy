#pragma once

#include <basalt/wire/auth/forwarding/ProxyForwarding.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::wire::common::toml {
class TomlTable;
}

namespace basalt::proxy::config {

enum class TranslationMode { Auto, Proxy, Off };

struct NetworkConfig {
    std::string bind = "0.0.0.0";
    std::uint16_t port = 25565;
    bool dualStack = false;
    bool onlineMode = true;
    bool acceptTransfers = false;
    bool preventClientProxyConnections = false;
    std::int32_t compressionThreshold = 256;
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
};

struct AdmissionConfig {
    std::uint32_t globalRate = 200;
    std::uint32_t globalBurst = 400;
    std::uint32_t originRate = 10;
    std::uint32_t originBurst = 20;
    std::size_t maxOrigins = 65536;
    std::uint32_t originExpiryMs = 60000;
};

struct BackendConfig {
    std::string name;
    std::string host;
    std::uint16_t port = 25566;
    std::int32_t protocol = 777;
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
    TranslationMode translation = TranslationMode::Auto;
    std::vector<BackendConfig> backends;
    std::vector<std::string> defaultRoute;
    std::vector<HostRoute> forcedHosts;
};

[[nodiscard]] std::string_view defaultProxyToml() noexcept;
[[nodiscard]] std::string_view defaultBackendsToml() noexcept;
[[nodiscard]] std::expected<ProxyConfig, std::string> fromToml(const wire::common::toml::TomlTable& proxy,
                                                               const wire::common::toml::TomlTable& backends,
                                                               std::string_view directory);
[[nodiscard]] std::expected<ProxyConfig, std::string> load(std::string_view directory);

} // namespace basalt::proxy::config
