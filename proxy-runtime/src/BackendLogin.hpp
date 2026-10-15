#pragma once

#include <basalt/wire/io/ByteBuffer.hpp>
#include <basalt/wire/io/EventLoop.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

#include <expected>
#include <span>
#include <string_view>

namespace basalt::proxy::config {
struct BackendConfig;
}

namespace basalt::wire::auth {
struct SessionProfile;
}

namespace basalt::proxy::runtime::detail {

enum class BackendLoginError { kUnsupported, kMalformed, kIo, kLimit };

/// Initial native login transport; all buffers and calls belong to the session's I/O owner.
class BackendLogin {
public:
    static constexpr std::size_t kCapacity = 1024;
    static constexpr std::size_t kBytes = 3 * kCapacity;
    explicit BackendLogin(wire::io::TcpConnection connection);
    [[nodiscard]] std::expected<void, BackendLoginError> start(const config::BackendConfig& backend,
                                                               const wire::auth::SessionProfile& profile,
                                                               std::string_view virtualHost, std::uint16_t virtualPort);
    /// Returned packet view expires on the next call. No frontend framing/cipher state is shared.
    [[nodiscard]] std::expected<std::span<const std::uint8_t>, BackendLoginError> ready(const wire::io::IoReady& event);

    [[nodiscard]] std::int64_t handle() const { return connection_.nativeHandle(); }

    [[nodiscard]] wire::io::IoInterest interest() const { return {true, output_.readableBytes() != 0}; }

    [[nodiscard]] std::size_t allocated() const;

private:
    wire::io::TcpConnection connection_;
    wire::io::ByteBuffer input_{kCapacity};
    wire::io::ByteBuffer output_{kCapacity};
    wire::io::ByteBuffer frame_{kCapacity};
    bool started_ = false;
};

} // namespace basalt::proxy::runtime::detail
