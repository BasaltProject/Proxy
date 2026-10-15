#pragma once

#include <basalt/wire/auth/Cfb8Cipher.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/io/BudgetMemoryResource.hpp>
#include <basalt/wire/io/ByteBuffer.hpp>
#include <basalt/wire/io/EventLoop.hpp>
#include <basalt/wire/io/ResourceBudget.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace basalt::proxy::config {
struct RuntimeConfig;
struct BackendConfig;
} // namespace basalt::proxy::config

namespace basalt::proxy::runtime::detail {

class BackendRoutes;
class BackendDial;
class BackendLogin;

class ClientSession {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kBufferCapacity = 1024;
    static constexpr std::size_t kBufferBytes = kBufferCapacity * 3;
    static constexpr std::size_t kAuthenticationBytes = 128 * 1024;
    static constexpr std::size_t kResolutionBytes = 16 * 1024;
    using AuthenticationStart = std::function<std::optional<std::stop_source>(wire::auth::OnlineLoginRequest)>;
    using ResolutionStart = std::function<std::optional<std::stop_source>(std::size_t, Clock::time_point)>;
    ClientSession(wire::io::TcpConnection connection, wire::io::ResourceBudget::Reservation slot,
                  wire::io::ResourceBudget::Reservation buffers, const config::RuntimeConfig& settings,
                  std::span<const std::uint8_t> status, std::span<const std::uint8_t> unavailable,
                  Clock::time_point acceptedAt, bool online, bool checkAddress, std::span<const std::uint8_t> publicKey,
                  std::span<const std::uint8_t> authFailure, AuthenticationStart authenticate,
                  const BackendRoutes& routes, ResolutionStart resolve,
                  const std::vector<config::BackendConfig>& backends, wire::io::ResourceBudget globalBuffers,
                  std::int32_t compressionThreshold);
    ~ClientSession();
    [[nodiscard]] bool
    authenticated(std::expected<wire::auth::AuthenticatedLogin, wire::auth::LoginAuthenticationFailure> result,
                  wire::io::ResourceBudget::Reservation storage);
    [[nodiscard]] bool resolved(std::size_t target, std::vector<wire::io::TcpAddress> addresses,
                                wire::io::ResourceBudget::Reservation storage);
    [[nodiscard]] bool backendReady(const wire::io::IoReady& event);
    [[nodiscard]] std::int64_t backendHandle() const;
    [[nodiscard]] wire::io::IoInterest backendInterest() const;

    [[nodiscard]] bool frontendSuspended() const { return frontendPaused_ && output_.readableBytes() == 0; }

    [[nodiscard]] bool backendSuspended() const;

    [[nodiscard]] bool ready(const wire::io::IoReady& event);
    [[nodiscard]] wire::io::IoInterest interest() const;
    [[nodiscard]] Clock::time_point deadline() const;

    [[nodiscard]] std::int64_t handle() const { return connection_.nativeHandle(); }

    [[nodiscard]] bool accounted() const;
    std::uint64_t timer = 0;

private:
    enum class State {
        Handshake,
        StatusRequest,
        Ping,
        LoginStart,
        Key,
        Authenticating,
        kResolving,
        kConnecting,
        kBackendLogin,
        kLoginAck,
        kConfiguration,
        kPlay
    };
    [[nodiscard]] bool decode();
    [[nodiscard]] bool packet();
    [[nodiscard]] bool queue(std::span<const std::uint8_t> bytes);
    [[nodiscard]] bool queuePacket(std::span<const std::uint8_t> packet);
    [[nodiscard]] bool route();
    [[nodiscard]] bool dial();
    void backpressure();
    [[nodiscard]] bool bufferedPacket() const;
    wire::io::ResourceBudget::Reservation slot_;
    wire::io::ResourceBudget localBuffers_;
    wire::io::BudgetMemoryResource memory_;
    bool allocationReady_ = false;
    wire::io::TcpConnection connection_;
    const config::RuntimeConfig& settings_;
    std::span<const std::uint8_t> status_;
    std::span<const std::uint8_t> unavailable_;
    bool online_;
    bool checkAddress_;
    std::int32_t compressionThreshold_;
    std::optional<std::int32_t> compression_;
    std::span<const std::uint8_t> publicKey_;
    std::span<const std::uint8_t> authFailure_;
    AuthenticationStart authenticate_;
    const BackendRoutes& routes_;
    std::string virtualHost_;
    std::uint16_t virtualPort_ = 0;
    std::span<const std::size_t> targets_;
    ResolutionStart resolve_;
    std::size_t nextTarget_ = 0;
    std::optional<std::stop_source> resolution_;
    std::optional<wire::io::ResourceBudget::Reservation> resolutionLocal_;
    std::optional<wire::io::ResourceBudget::Reservation> resolutionStorage_;
    std::unique_ptr<BackendDial> dial_;
    const std::vector<config::BackendConfig>& backends_;
    std::unique_ptr<BackendLogin> backend_;
    Clock::time_point backendWriteDeadline_ = Clock::time_point::max();
    Clock::time_point backendReadDeadline_ = Clock::time_point::max();
    bool finishingConfiguration_ = false;
    Clock::time_point connectDeadline_;
    std::array<std::uint8_t, 4> challenge_{};
    std::string name_;
    std::optional<std::stop_source> authentication_;
    std::optional<wire::io::ResourceBudget::Reservation> profileStorage_;
    std::optional<wire::io::ResourceBudget::Reservation> profileLocal_;
    std::optional<wire::auth::SessionProfile> profile_;
    std::optional<wire::auth::Cfb8Cipher> encryptor_;
    std::optional<wire::auth::Cfb8Cipher> decryptor_;
    wire::io::ByteBuffer input_;
    wire::io::ByteBuffer frame_;
    wire::io::ByteBuffer output_;
    State state_ = State::Handshake;
    bool closing_ = false;
    bool backendPaused_ = false;
    bool frontendPaused_ = false;
    Clock::time_point readDeadline_;
    Clock::time_point writeDeadline_ = Clock::time_point::max();
    Clock::time_point loginDeadline_;
};

} // namespace basalt::proxy::runtime::detail
