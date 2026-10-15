#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace basalt::proxy::config {
struct ProxyConfig;
}

namespace basalt::proxy::runtime::detail {

/// Immutable lookup for a configuration snapshot. Borrowed routes live as long as the snapshot.
class BackendRoutes {
public:
    [[nodiscard]] static std::expected<BackendRoutes, std::string> create(const config::ProxyConfig& config);
    [[nodiscard]] std::span<const std::size_t> select(std::string_view virtualHost) const;
    [[nodiscard]] std::optional<std::size_t> named(std::string_view backend) const;

private:
    std::unordered_map<std::string, std::size_t> names_;
    std::vector<std::size_t> defaults_;
    std::unordered_map<std::string, std::vector<std::size_t>> hosts_;
};

} // namespace basalt::proxy::runtime::detail
