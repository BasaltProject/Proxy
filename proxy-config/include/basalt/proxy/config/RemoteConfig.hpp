#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace basalt::wire::common::toml {
class TomlTable;
}

namespace basalt::proxy::config {

struct RemoteApiConfig {
    bool enabled = false;
    std::string bind = "127.0.0.1";
    std::uint16_t port = 25590;
    std::int32_t maxConnections = 32;
    std::int32_t maxConnectionsPerAddress = 8;
    std::int32_t maxBodyBytes = 65536;
    std::int32_t idleSeconds = 60;
    std::int32_t maxFailures = 5;
    std::int32_t lockoutSeconds = 30;
    std::int32_t requestsPerMinute = 120;

    friend bool operator==(const RemoteApiConfig&, const RemoteApiConfig&) = default;
};

struct RemoteActionsConfig {
    bool send = true;
    bool kick = true;
    bool reload = true;

    friend bool operator==(const RemoteActionsConfig&, const RemoteActionsConfig&) = default;
};

struct RemoteConfig {
    RemoteApiConfig api;
    RemoteActionsConfig actions;

    friend bool operator==(const RemoteConfig&, const RemoteConfig&) = default;
};

[[nodiscard]] std::expected<RemoteConfig, std::string> remoteFromToml(const wire::common::toml::TomlTable& table,
                                                                      std::string_view directory);
[[nodiscard]] std::expected<RemoteConfig, std::string> loadRemote(std::string_view directory);

} // namespace basalt::proxy::config
