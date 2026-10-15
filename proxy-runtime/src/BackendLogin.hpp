#pragma once

#include "BackendAttempts.hpp"

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

namespace basalt::wire::protocol::login {
struct LoginFinishedPacket;
}

namespace basalt::proxy::runtime::detail {

class CookieCache;

enum class BackendLoginError { kUnsupported, kMalformed, kIo, kLimit };

/// Native backend packet transport; all buffers and calls belong to the session's I/O owner.
class BackendLogin {
public:
    enum class CookieAction { kCached, kRequestClient };
    enum class Phase { kUnstarted, kLogin, kLoginAccepted, kConfiguration, kPlay };

    [[nodiscard]] Phase phase() const { return phase_; }

    static constexpr std::size_t kCapacity = 1024;
    static constexpr std::size_t kBytes = 3 * kCapacity;
    static constexpr std::size_t kLoginPacketLimit = 64 * 1024;
    static constexpr std::size_t kForwardingPacketLimit = 128 * 1024;
    BackendLogin(wire::io::TcpConnection connection, std::pmr::memory_resource& memory, BackendAttempt attempt);

    [[nodiscard]] BackendAttempt attempt() const { return attempt_; }

    [[nodiscard]] bool velocityForwarded() const { return velocityForwarded_; }

    [[nodiscard]] bool hasCookieRequest() const { return cookieKey_.readableBytes() != 0; }

    [[nodiscard]] std::expected<CookieAction, BackendLoginError> requestCookie(wire::io::ByteBuffer& packet,
                                                                               const CookieCache& cache);
    [[nodiscard]] std::expected<void, BackendLoginError> answerCookie(wire::io::ByteBuffer& packet, CookieCache& cache);

    [[nodiscard]] std::expected<void, BackendLoginError> start(const config::BackendConfig& backend,
                                                               const wire::auth::SessionProfile& profile,
                                                               std::string_view virtualHost, std::uint16_t virtualPort,
                                                               std::string_view remoteAddress);
    /// Query begins after its packet ID. Replies affect only this transport.
    [[nodiscard]] std::expected<void, BackendLoginError> answerPluginQuery(wire::io::ByteBuffer& query,
                                                                           const config::BackendConfig& backend,
                                                                           const wire::auth::SessionProfile& profile,
                                                                           std::string_view remoteAddress);

    /// Returned packet view expires on the next call. No frontend framing/cipher state is shared.
    [[nodiscard]] std::expected<std::span<const std::uint8_t>, BackendLoginError> ready(const wire::io::IoReady& event,
                                                                                        bool read = true);
    [[nodiscard]] std::expected<void, BackendLoginError> queue(std::span<const std::uint8_t> packet,
                                                               std::size_t loginLimit = kCapacity);

    [[nodiscard]] std::int64_t handle() const { return connection_.nativeHandle(); }

    [[nodiscard]] wire::io::IoInterest interest(bool read = true) const;

    [[nodiscard]] std::size_t pendingBytes() const { return output_.readableBytes(); }

    [[nodiscard]] bool hasOutput() const { return output_.readableBytes() != 0; }

    /// Packet begins after its ID. Identity is checked before this transport accepts login.
    [[nodiscard]] std::expected<wire::protocol::login::LoginFinishedPacket, BackendLoginError>
    acceptLogin(wire::io::ByteBuffer& packet, const config::BackendConfig& backend,
                const wire::auth::SessionProfile& profile);
    [[nodiscard]] bool acknowledgeLogin();
    [[nodiscard]] bool beginConfiguration();
    [[nodiscard]] bool finishConfiguration();

    [[nodiscard]] std::size_t allocated() const;

private:
    [[nodiscard]] bool bufferedPacket() const;
    [[nodiscard]] bool inLogin() const;
    [[nodiscard]] std::expected<void, BackendLoginError>
    queueCookie(std::string_view key, std::optional<std::span<const std::uint8_t>> payload);
    [[nodiscard]] bool advance(Phase from, Phase to);
    const BackendAttempt attempt_;
    bool velocityForwarded_ = false;
    wire::io::TcpConnection connection_;
    wire::io::ByteBuffer cookieKey_;
    std::uint64_t cookieRevision_ = 0;
    wire::io::ByteBuffer input_;
    wire::io::ByteBuffer output_;
    wire::io::ByteBuffer frame_;
    bool allocationReady_ = false;
    Phase phase_ = Phase::kUnstarted;
    bool compressionNegotiated_ = false;
    std::optional<std::int32_t> compression_;
};

} // namespace basalt::proxy::runtime::detail
