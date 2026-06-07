#include "ConfigReader.hpp"

#include <basalt/proxy/config/ConfigDefaults.hpp>
#include <basalt/proxy/config/RemoteConfig.hpp>
#include <basalt/wire/io/ConfigFiles.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

namespace basalt::proxy::config {

std::expected<RemoteConfig, std::string> remoteFromToml(const detail::TomlTable& table, std::string_view directory) {
    RemoteConfig result;
    detail::Reader reader{std::string{directory} + "/remote.toml", {}};
    reader.keys(&table, {"api", "actions"}, "");
    const auto* api = reader.child(table, "api");
    reader.keys(api,
                {"enabled", "bind", "port", "max-connections", "max-connections-per-address", "max-body-bytes",
                 "idle-seconds", "max-failures", "lockout-seconds", "requests-per-minute"},
                "api.");
    reader.scalar(api, "enabled", result.api.enabled, "api.");
    reader.scalar(api, "bind", result.api.bind, "api.");
    reader.integer(api, "port", result.api.port, 1, 65535, "api.");
    reader.integer(api, "max-connections", result.api.maxConnections, 1, 4096, "api.");
    reader.integer(api, "max-connections-per-address", result.api.maxConnectionsPerAddress, 1, 4096, "api.");
    reader.integer(api, "max-body-bytes", result.api.maxBodyBytes, 1, 16 * 1024 * 1024, "api.");
    reader.integer(api, "idle-seconds", result.api.idleSeconds, 1, 3600, "api.");
    reader.integer(api, "max-failures", result.api.maxFailures, 1, 1000, "api.");
    reader.integer(api, "lockout-seconds", result.api.lockoutSeconds, 1, 86400, "api.");
    reader.integer(api, "requests-per-minute", result.api.requestsPerMinute, 1, 1000000, "api.");
    if (!wire::io::TcpAddress::literal(result.api.bind, result.api.port))
        reader.fail("api.bind", "must be a literal IPv4 or IPv6 address");

    const auto* actions = reader.child(table, "actions");
    reader.keys(actions, {"send", "kick", "reload"}, "actions.");
    reader.scalar(actions, "send", result.actions.send, "actions.");
    reader.scalar(actions, "kick", result.actions.kick, "actions.");
    reader.scalar(actions, "reload", result.actions.reload, "actions.");
    if (reader.error)
        return std::unexpected(*reader.error);
    return result;
}

std::expected<RemoteConfig, std::string> loadRemote(std::string_view directory) {
    auto document = wire::io::configFiles::loadToml(directory, "remote.toml", kRemoteDefaultToml);
    if (!document)
        return std::unexpected(document.error());
    return remoteFromToml(document->values, directory);
}

} // namespace basalt::proxy::config
