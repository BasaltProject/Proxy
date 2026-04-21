#include "ConfigReader.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

#include <limits>

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

void validateRoute(Reader& reader, const ProxyConfig& config, const std::vector<std::string>& names,
                   std::string_view field) {
    for (const auto& name : names) {
        if (std::ranges::none_of(config.backends, [&](const auto& backend) { return backend.name == name; }))
            reader.fail(field, "references an unknown backend");
    }
}
} // namespace

std::expected<void, std::string> readBackends(ProxyConfig& config, const TomlTable& root, std::string_view path) {
    Reader reader{std::string{path}, {}};
    reader.keys(&root, {"backends", "routing", "forced-hosts"}, "");
    const auto* entries = reader.child(root, "backends");
    if (!entries || entries->size() == 0 || entries->size() > 256)
        reader.fail("backends", "must contain 1..256 named tables");
    if (entries) {
        for (const auto& name : entries->keys()) {
            if (!validName(name)) {
                reader.fail("backends", "contains an invalid backend name");
                continue;
            }
            const auto* table = reader.child(*entries, name);
            if (!table)
                continue;
            const auto prefix = "backends." + name + ".";
            reader.keys(table, {"host", "port", "protocol", "forwarding", "secret", "property-name", "bungee-messages"},
                        prefix);
            BackendConfig backend;
            backend.name = name;
            reader.scalar(table, "bungee-messages", backend.bungeeMessages, prefix);
            reader.scalar(table, "host", backend.host, prefix);
            reader.integer(table, "port", backend.port, 1, 65535, prefix);
            reader.integer(table, "protocol", backend.protocol, 1, std::numeric_limits<std::int32_t>::max(), prefix);
            reader.scalar(table, "secret", backend.secret, prefix);
            reader.scalar(table, "property-name", backend.propertyName, prefix);
            std::string forwarding = "none";
            if (!table->find("forwarding"))
                reader.fail(prefix + "forwarding", "must be chosen explicitly for this backend");
            reader.scalar(table, "forwarding", forwarding, prefix);
            const auto parsed = wire::auth::forwarding::parseForwardingMode(forwarding);
            if (!parsed)
                reader.fail(prefix + "forwarding", "has an unknown scheme");
            else
                backend.forwarding = *parsed;
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
    const auto* routing = reader.child(root, "routing");
    reader.keys(routing, {"default"}, "routing.");
    config.defaultRoute = reader.strings(routing ? routing->find("default") : nullptr, "routing.default");
    validateRoute(reader, config, config.defaultRoute, "routing.default");
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
            auto route = reader.strings(forced->find(written), "forced-hosts");
            validateRoute(reader, config, route, "forced-hosts");
            config.forcedHosts.push_back({std::move(host), std::move(route)});
        }
    }
    if (reader.error)
        return std::unexpected(*reader.error);
    return {};
}

} // namespace basalt::proxy::config::detail
