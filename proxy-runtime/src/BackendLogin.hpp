#pragma once

#include <basalt/wire/io/ByteBuffer.hpp>
#include <basalt/wire/io/EventLoop.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

#include <expected>
#include <optional>
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

/// Native backend packet transport; all buffers and calls belong to the session's I/O owner.
class BackendLogin {
public:
    static constexpr std::size_t kCapacity = 1024;
    static constexpr std::size_t kBytes = 3 * kCapacity;
    BackendLogin(wire::io::TcpConnection connection, std::pmr::memory_resource& memory);
    [[nodiscard]] std::expected<void, BackendLoginError> start(const config::BackendConfig& backend,
                                                               const wire::auth::SessionProfile& profile,
                                                               std::string_view virtualHost, std::uint16_t virtualPort);
    /// Returned packet view expires on the next call. No frontend framing/cipher state is shared.
    [[nodiscard]] std::expected<std::span<const std::uint8_t>, BackendLoginError> ready(const wire::io::IoReady& event,
                                                                                        bool read = true);
    [[nodiscard]] std::expected<void, BackendLoginError> queue(std::span<const std::uint8_t> packet);

    [[nodiscard]] std::int64_t handle() const { return connection_.nativeHandle(); }

    [[nodiscard]] wire::io::IoInterest interest(bool read = true) const;

    [[nodiscard]] std::size_t pendingBytes() const { return output_.readableBytes(); }

    [[nodiscard]] bool hasOutput() const { return output_.readableBytes() != 0; }

    void acknowledgeLogin() { login_ = false; }

    [[nodiscard]] std::size_t allocated() const;

private:
    [[nodiscard]] bool bufferedPacket() const;
    wire::io::TcpConnection connection_;
    wire::io::ByteBuffer input_;
    wire::io::ByteBuffer output_;
    wire::io::ByteBuffer frame_;
    bool allocationReady_ = false;
    bool started_ = false;
    bool compressionNegotiated_ = false;
    bool login_ = true;
    std::optional<std::int32_t> compression_;
};

} // namespace basalt::proxy::runtime::detail
