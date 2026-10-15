#pragma once

#include <basalt/proxy/runtime/BackendChangeRequest.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/io/DnsResolver.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::config {
struct ProxyConfig;
}

namespace basalt::proxy::runtime {

struct RuntimeServices {
    wire::auth::AuthenticationFetch authentication;
    std::function<std::expected<std::vector<wire::io::TcpAddress>, wire::io::DnsError>(std::string_view, std::uint16_t,
                                                                                       const wire::io::DnsOptions&)>
        resolution = {};
};

class ProxyRuntime {
public:
    [[nodiscard]] static std::expected<ProxyRuntime, std::string> create(config::ProxyConfig config,
                                                                         RuntimeServices services = {});
    ProxyRuntime(ProxyRuntime&&) noexcept;
    ProxyRuntime& operator=(ProxyRuntime&&) noexcept;
    ~ProxyRuntime();
    ProxyRuntime(const ProxyRuntime&) = delete;
    ProxyRuntime& operator=(const ProxyRuntime&) = delete;

    /// Coordinator only; starts owners and joins them after stop or a fatal transport failure.
    [[nodiscard]] std::expected<void, std::string> run(std::stop_token stop);
    /// Thread-safe; matches the verified name exactly. Ticket retains bounded resources until released.
    [[nodiscard]] std::expected<BackendChangeRequest, BackendChangeError> sendPlayer(std::string_view player,
                                                                                     std::string_view backend);
    [[nodiscard]] std::uint16_t port() const noexcept;
    /// Counts admitted sessions, including bounded handoffs waiting for their owner.
    [[nodiscard]] std::size_t sessionCount() const noexcept;
    [[nodiscard]] std::size_t bufferedBytes() const noexcept;

private:
    struct Impl;
    explicit ProxyRuntime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace basalt::proxy::runtime
