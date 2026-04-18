#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>

namespace basalt::proxy::config {
struct ProxyConfig;
}

namespace basalt::proxy::runtime {

class ProxyRuntime {
public:
    [[nodiscard]] static std::expected<ProxyRuntime, std::string> create(config::ProxyConfig config);
    ProxyRuntime(ProxyRuntime&&) noexcept;
    ProxyRuntime& operator=(ProxyRuntime&&) noexcept;
    ~ProxyRuntime();
    ProxyRuntime(const ProxyRuntime&) = delete;
    ProxyRuntime& operator=(const ProxyRuntime&) = delete;

    /// Coordinator only; starts owners and joins them after stop or a fatal transport failure.
    [[nodiscard]] std::expected<void, std::string> run(std::stop_token stop);
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
