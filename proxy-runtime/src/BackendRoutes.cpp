#include "BackendRoutes.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>

#include <algorithm>

namespace basalt::proxy::runtime::detail {
namespace {
std::string normalize(std::string_view host) {
    if (host.empty() || host.size() > 255 || host.find('\0') != std::string_view::npos)
        return {};
    std::string result{host};
    for (char& c : result) {
        if (static_cast<unsigned char>(c) <= 32 || c == 127)
            return {};
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    }
    if (result.ends_with('.'))
        result.pop_back();
    return result;
}
} // namespace

std::expected<BackendRoutes, std::string> BackendRoutes::create(const config::ProxyConfig& config) {
    BackendRoutes result;
    if (config.backends.size() > 256 || config.forcedHosts.size() > 256)
        return std::unexpected("routing snapshot exceeds its size limit");
    for (std::size_t i = 0; i < config.backends.size(); ++i) {
        const auto& name = config.backends[i].name;
        if (name.empty() || !result.names_.emplace(name, i).second)
            return std::unexpected("routing snapshot has duplicate or empty backend names");
    }
    const auto resolve =
        [&](const std::vector<std::string>& names) -> std::expected<std::vector<std::size_t>, std::string> {
        if (names.empty() || names.size() > 256)
            return std::unexpected("routing list must contain 1..256 backend names");
        std::vector<std::size_t> indices;
        indices.reserve(names.size());
        for (const auto& name : names) {
            const auto found = result.names_.find(name);
            if (found == result.names_.end())
                return std::unexpected("routing list refers to an unknown backend");
            if (std::ranges::find(indices, found->second) != indices.end())
                return std::unexpected("routing list contains a duplicate backend");
            indices.push_back(found->second);
        }
        return indices;
    };
    if (!config.backends.empty() || !config.defaultRoute.empty()) {
        auto defaults = resolve(config.defaultRoute);
        if (!defaults)
            return std::unexpected(defaults.error());
        result.defaults_ = std::move(*defaults);
    }
    for (const auto& route : config.forcedHosts) {
        const auto host = normalize(route.host);
        if (host.empty())
            return std::unexpected("routing snapshot has an invalid virtual host");
        auto indices = resolve(route.backends);
        if (!indices)
            return std::unexpected(indices.error());
        if (!result.hosts_.emplace(host, std::move(*indices)).second)
            return std::unexpected("routing snapshot has duplicate normalized virtual hosts");
    }
    return result;
}

std::span<const std::size_t> BackendRoutes::select(std::string_view virtualHost) const {
    const auto host = normalize(virtualHost);
    if (host.empty())
        return {};
    const auto found = hosts_.find(host);
    return found == hosts_.end() ? std::span<const std::size_t>{defaults_} : found->second;
}

std::optional<std::size_t> BackendRoutes::named(std::string_view backend) const {
    const auto found = names_.find(std::string{backend});
    return found == names_.end() ? std::nullopt : std::optional{found->second};
}

} // namespace basalt::proxy::runtime::detail
