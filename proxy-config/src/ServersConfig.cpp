#include "ConfigReader.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

#include <functional>
#include <limits>
#include <map>

namespace basalt::proxy::config::detail {
namespace {
bool validName(std::string_view name) {
    return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

bool validHost(std::string_view host) {
    if (host.find(':') != std::string_view::npos)
        return wire::io::TcpAddress::literal(host, 1).has_value();
    if (host.empty() || host.front() == '.' || host.find("..") != std::string_view::npos ||
        host.find('%') != std::string_view::npos)
        return false;
    return !host.empty() && host.size() <= 253 && std::ranges::all_of(host, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
               c == '_' || c == ':' || c == '%';
    });
}

using Groups = std::map<std::string, std::vector<std::string>, std::less<>>;

std::vector<std::string> expandRoute(Reader& reader, const ProxyConfig& config, const Groups& groups,
                                     const std::vector<std::string>& names, std::string_view field) {
    std::vector<std::string> route;
    const auto add = [&](const std::string& server) {
        if (std::ranges::find(route, server) == route.end())
            route.push_back(server);
    };
    for (const auto& name : names) {
        if (const auto group = groups.find(name); group != groups.end()) {
            std::ranges::for_each(group->second, add);
        } else if (std::ranges::any_of(config.backends, [&](const auto& server) { return server.name == name; })) {
            add(name);
        } else {
            reader.fail(field, "references an unknown server or group");
        }
    }
    return route;
}
} // namespace

std::expected<void, std::string> readServers(ProxyConfig& config, const TomlTable& root, std::string_view path) {
    Reader reader{std::string{path}, {}};
    reader.keys(&root, {"servers", "server-groups", "routing", "forced-hosts", "health", "status-relay"}, "");
    const auto* entries = reader.child(root, "servers");
    if (!entries || entries->size() == 0 || entries->size() > 256)
        reader.fail("servers", "must contain 1..256 named tables");
    if (entries) {
        for (const auto& name : entries->keys()) {
            if (!validName(name)) {
                reader.fail("servers", "contains an invalid server name");
                continue;
            }
            const auto* table = reader.child(*entries, name);
            if (!table)
                continue;
            const auto prefix = "servers." + name + ".";
            reader.keys(table, {"host", "port", "protocol", "forwarding", "secret", "property-name", "bungee-messages"},
                        prefix);
            BackendConfig backend;
            backend.name = name;
            reader.scalar(table, "bungee-messages", backend.bungeeMessages, prefix);
            reader.scalar(table, "host", backend.host, prefix);
            reader.integer(table, "port", backend.port, 1, 65535, prefix);
            bool automatic = false;
            if (const auto* declared = table->find("protocol"); declared && declared->get<std::string>()) {
                automatic = *declared->get<std::string>() == "auto";
                if (!automatic && *declared->get<std::string>() != "client")
                    reader.fail(prefix + "protocol", "must be a protocol number, \"client\" or \"auto\"");
                backend.protocol = BackendConfig::kClientProtocol;
            } else {
                reader.integer(table, "protocol", backend.protocol, 1, std::numeric_limits<std::int32_t>::max(),
                               prefix);
            }
            reader.scalar(table, "secret", backend.secret, prefix);
            reader.scalar(table, "property-name", backend.propertyName, prefix);
            std::string forwarding = "none";
            if (!table->find("forwarding"))
                reader.fail(prefix + "forwarding", "must be chosen explicitly for this server");
            reader.scalar(table, "forwarding", forwarding, prefix);
            const auto parsed = wire::auth::forwarding::parseForwardingMode(forwarding);
            if (!parsed)
                reader.fail(prefix + "forwarding", "has an unknown scheme");
            else
                backend.forwarding = *parsed;
            backend.automatic = automatic;
            if (automatic)
                backend.protocol = backend.forwarding == wire::auth::forwarding::ForwardingMode::Basalt
                                       ? BackendConfig::kClientProtocol
                                       : BackendConfig{}.protocol;
            if (!validHost(backend.host))
                reader.fail(prefix + "host", "must be a hostname or literal IP address without a port");
            using Mode = wire::auth::forwarding::ForwardingMode;
            const bool requiresSecret = backend.forwarding == Mode::Velocity ||
                                        backend.forwarding == Mode::BungeeGuard || backend.forwarding == Mode::SafeNet;
            if (requiresSecret && (backend.secret.empty() || backend.secret.size() > 4096 ||
                                   backend.secret.find('\0') != std::string::npos))
                reader.fail(prefix + "secret", "must contain 1..4096 bytes without NUL for this scheme");
            if (!requiresSecret && !backend.secret.empty())
                reader.fail(prefix + "secret", "is unused by this scheme; remove it or "
                                               "choose the matching scheme");
            if (table->find("property-name") && backend.forwarding != Mode::SafeNet)
                reader.fail(prefix + "property-name", "is only used by safenet");
            if (backend.propertyName.empty() || backend.propertyName.size() > 256 ||
                std::ranges::any_of(backend.propertyName, [](char c) { return static_cast<unsigned char>(c) <= 32; }))
                reader.fail(prefix + "property-name", "must be a nonempty property name of at most 256 bytes");
            config.backends.push_back(std::move(backend));
        }
    }
    Groups groups;
    const auto* groupTables = reader.child(root, "server-groups");
    if (groupTables && groupTables->size() > 256)
        reader.fail("server-groups", "exceeds 256 groups");
    if (groupTables) {
        for (const auto& name : groupTables->keys()) {
            const auto prefix = "server-groups." + name + ".";
            if (!validName(name) ||
                std::ranges::any_of(config.backends, [&](const auto& server) { return server.name == name; })) {
                reader.fail("server-groups", "contains an invalid group name or one shared with a server");
                continue;
            }
            const auto* table = reader.child(*groupTables, name);
            if (!table)
                continue;
            reader.keys(table, {"servers"}, prefix);
            const auto members = reader.strings(table->find("servers"), prefix + "servers");
            groups[name] = expandRoute(reader, config, {}, members, prefix + "servers");
            config.serverGroups.push_back({name, groups[name]});
        }
    }
    const auto* routing = reader.child(root, "routing");
    reader.keys(routing, {"try"}, "routing.");
    config.defaultRoute = expandRoute(
        reader, config, groups, reader.strings(routing ? routing->find("try") : nullptr, "routing.try"), "routing.try");
    const auto* health = reader.child(root, "health");
    reader.keys(health, {"enabled", "mode", "interval-seconds", "timeout-ms", "failures-before-down"}, "health.");
    reader.scalar(health, "enabled", config.health.enabled, "health.");
    std::string mode = "tcp";
    reader.scalar(health, "mode", mode, "health.");
    if (mode == "tcp")
        config.health.mode = HealthMode::kTcp;
    else if (mode == "status")
        config.health.mode = HealthMode::kStatus;
    else
        reader.fail("health.mode", "must be \"tcp\" or \"status\"");
    reader.integer(health, "interval-seconds", config.health.intervalSeconds, 1, 3600, "health.");
    reader.integer(health, "timeout-ms", config.health.timeoutMs, 50, 60000, "health.");
    reader.integer(health, "failures-before-down", config.health.failuresBeforeDown, 1, 100, "health.");
    const auto* relay = reader.child(root, "status-relay");
    reader.keys(relay, {"ttl-ms", "timeout-ms"}, "status-relay.");
    reader.integer(relay, "ttl-ms", config.statusRelay.ttlMs, 100, 3600000, "status-relay.");
    reader.integer(relay, "timeout-ms", config.statusRelay.timeoutMs, 50, 60000, "status-relay.");
    const auto* forced = reader.child(root, "forced-hosts");
    if (forced && forced->size() > 256)
        reader.fail("forced-hosts", "exceeds 256 routes");
    if (forced) {
        for (const auto& written : forced->keys()) {
            if (!validHost(written) || written.find_first_of(":%") != std::string::npos) {
                reader.fail("forced-hosts", "contains an invalid virtual hostname");
                continue;
            }
            std::string host = written;
            for (char& c : host) {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c + ('a' - 'A'));
            }
            if (host.ends_with('.'))
                host.pop_back();
            if (std::ranges::any_of(config.forcedHosts, [&](const auto& route) { return route.host == host; }))
                reader.fail("forced-hosts", "contains duplicate normalized hosts");
            auto route = expandRoute(reader, config, groups, reader.strings(forced->find(written), "forced-hosts"),
                                     "forced-hosts");
            config.forcedHosts.push_back({std::move(host), std::move(route)});
        }
    }
    if (reader.error)
        return std::unexpected(*reader.error);
    return {};
}

} // namespace basalt::proxy::config::detail
