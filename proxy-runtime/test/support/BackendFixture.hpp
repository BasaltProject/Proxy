#pragma once

#include <basalt/wire/io/TcpSocket.hpp>

#include <cstdint>
#include <expected>
#include <utility>

namespace basalt::proxy::runtime::test {

class BackendFixture {
public:
    [[nodiscard]] static std::expected<BackendFixture, wire::io::SocketError> listen() {
        auto listener = wire::io::TcpListener::bind(0);
        if (!listener) {
            return std::unexpected{listener.error()};
        }
        const auto port = listener->localPort();
        if (!port) {
            return std::unexpected{port.error()};
        }
        const auto mode = listener->setNonBlocking();
        if (!mode) {
            return std::unexpected{mode.error()};
        }
        return BackendFixture{std::move(*listener), *port};
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] std::int64_t handle() const noexcept { return listener_.nativeHandle(); }

    [[nodiscard]] std::expected<wire::io::TcpConnection, wire::io::SocketError> accept() { return listener_.accept(); }

    void close() { listener_.close(); }

private:
    BackendFixture(wire::io::TcpListener listener, std::uint16_t port) : listener_(std::move(listener)), port_(port) {}

    wire::io::TcpListener listener_;
    std::uint16_t port_;
};

} // namespace basalt::proxy::runtime::test
