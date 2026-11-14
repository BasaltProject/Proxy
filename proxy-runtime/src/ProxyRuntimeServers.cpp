#include "ProxyRuntimeImpl.hpp"

#include <basalt/wire/common/logging/Logger.hpp>

namespace basalt::proxy::runtime {
namespace {
constexpr std::size_t kMaxServerName = 64;

bool validName(std::string_view name) {
    return !name.empty() && name.size() <= kMaxServerName && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.';
    });
}
} // namespace

std::expected<void, std::string>
ProxyRuntime::Impl::publishTopology(std::shared_ptr<const detail::NativeForwarding> forwarding) {
    auto merged = routing;
    std::erase_if(dynamicBackends, [&](const config::BackendConfig& server) {
        const bool clash = std::ranges::contains(merged.backends, server.name, &config::BackendConfig::name);
        if (clash)
            wire::common::Logger{"Servers"}.warn([&] {
                return "Server '" + server.name + "' is in servers.toml now; the one a plugin registered is gone.";
            });
        return clash;
    });
    merged.backends.insert(merged.backends.end(), dynamicBackends.begin(), dynamicBackends.end());
    auto routes = detail::BackendRoutes::create(merged);
    if (!routes)
        return std::unexpected(routes.error());
    topology.store(std::make_shared<const Topology>(Topology{std::move(merged.backends), std::move(merged.serverGroups),
                                                             std::move(*routes), std::move(forwarding)}));
    return {};
}

std::expected<void, std::string> ProxyRuntime::registerServer(std::string name, std::string host, std::uint16_t port,
                                                              std::string_view like) {
    if (!validName(name))
        return std::unexpected("a server name is 1 to 64 letters, digits, '_', '-' or '.'");
    if (host.empty() || host.size() > 255 || port == 0)
        return std::unexpected("a server needs a host and a port");
    std::lock_guard lock{impl_->dynamicMutex};
    const auto current = impl_->topology.load();
    if (std::ranges::contains(current->backends, name, &config::BackendConfig::name))
        return std::unexpected("there is already a server called '" + name + "'");
    const config::BackendConfig* model = nullptr;
    if (!like.empty()) {
        const auto found = std::ranges::find(current->backends, like, &config::BackendConfig::name);
        if (found == current->backends.end())
            return std::unexpected("there is no server called '" + std::string{like} + "' to copy");
        model = &*found;
    } else if (const auto defaults = current->routes.select("bedrock.invalid"); !defaults.empty()) {
        model = &current->backends[defaults.front()];
    }
    config::BackendConfig server = model != nullptr ? *model : config::BackendConfig{};
    server.name = std::move(name);
    server.host = std::move(host);
    server.port = port;
    impl_->dynamicBackends.push_back(server);
    if (auto published = impl_->publishTopology(current->forwarding); !published) {
        impl_->dynamicBackends.pop_back();
        return published;
    }
    return {};
}

bool ProxyRuntime::unregisterServer(std::string_view name) {
    std::lock_guard lock{impl_->dynamicMutex};
    const auto found = std::ranges::find(impl_->dynamicBackends, name, &config::BackendConfig::name);
    if (found == impl_->dynamicBackends.end())
        return false;
    const auto removed = *found;
    impl_->dynamicBackends.erase(found);
    if (!impl_->publishTopology(impl_->topology.load()->forwarding)) {
        impl_->dynamicBackends.push_back(removed);
        return false;
    }
    return true;
}

bool ProxyRuntime::dynamicServer(std::string_view name) const {
    std::lock_guard lock{impl_->dynamicMutex};
    return std::ranges::contains(impl_->dynamicBackends, name, &config::BackendConfig::name);
}

} // namespace basalt::proxy::runtime
