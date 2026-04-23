#include "BackendLogin.hpp"

#include "NativeForwarding.hpp"
#include "RouteVersions.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/auth/forwarding/LegacyForwarding.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/handshake/HandshakePacket.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>

#include <algorithm>
#include <array>

namespace basalt::proxy::runtime::detail {

BackendLogin::BackendLogin(wire::io::TcpConnection connection, std::pmr::memory_resource& memory,
                           BackendAttempt attempt)
    : memory_(memory), attempt_(attempt), connection_(std::move(connection)), cookieKey_(0, memory), input_(0, memory),
      output_(0, memory), frame_(0, memory) {
    allocationReady_ = input_.tryReserve(kCapacity) && output_.tryReserve(kCapacity) && frame_.tryReserve(kCapacity);
}

std::expected<void, BackendLoginError> BackendLogin::start(const config::BackendConfig& backend,
                                                           const wire::auth::SessionProfile& profile,
                                                           std::string_view virtualHost, std::uint16_t virtualPort,
                                                           std::string_view remoteAddress, std::uint16_t remotePort,
                                                           const NativeForwarding* native) {
    namespace protocol = wire::protocol;
    if (!allocationReady_)
        return std::unexpected(BackendLoginError::kLimit);
    if (phase_ != Phase::kUnstarted || !resolveRoute(buildInfo::kProtocolVersion, backend.protocol) ||
        (backend.forwarding != wire::auth::forwarding::ForwardingMode::None &&
         backend.forwarding != wire::auth::forwarding::ForwardingMode::Velocity &&
         backend.forwarding != wire::auth::forwarding::ForwardingMode::Basalt &&
         !wire::auth::forwarding::isLegacyForwarding(backend.forwarding)))
        return std::unexpected(BackendLoginError::kUnsupported);
    if (backend.forwarding == wire::auth::forwarding::ForwardingMode::Velocity && backend.secret.empty())
        return std::unexpected(BackendLoginError::kUnsupported);
    if (backend.forwarding == wire::auth::forwarding::ForwardingMode::Basalt && (!native || native->proxyId.empty()))
        return std::unexpected(BackendLoginError::kUnsupported);
    if (!wire::auth::validLoginName(profile.name) || virtualHost.empty() || virtualHost.size() > 255 ||
        virtualHost.find('\0') != std::string_view::npos)
        return std::unexpected(BackendLoginError::kMalformed);
    const std::string_view virtualHostText = virtualHost;
    std::optional<std::string> forwardedHost;
    if (wire::auth::forwarding::isLegacyForwarding(backend.forwarding)) {
        using Mode = wire::auth::forwarding::ForwardingMode;
        std::string_view tokenProperty;
        if (backend.forwarding == Mode::BungeeGuard)
            tokenProperty = wire::auth::forwarding::ProxyForwarding::kBungeeGuardProperty;
        else if (backend.forwarding == Mode::SafeNet)
            tokenProperty = backend.propertyName;
        if (backend.forwarding != Mode::BungeeCord && (tokenProperty.empty() || backend.secret.empty()))
            return std::unexpected(BackendLoginError::kUnsupported);
        forwardedHost = wire::auth::forwarding::legacy::encodeBounded(
            virtualHost, remoteAddress, profile.id, profile.properties, 32767, tokenProperty,
            tokenProperty.empty() ? std::string_view{} : std::string_view{backend.secret});
        if (!forwardedHost)
            return std::unexpected(BackendLoginError::kLimit);
        virtualHost = *forwardedHost;
    }
    if (!frame_.tryReserve(virtualHost.size() + 64) || !output_.tryReserve(virtualHost.size() + 128))
        return std::unexpected(BackendLoginError::kLimit);
    protocol::varInt::write(frame_, protocol::handshake::HandshakePacket::kId);
    protocol::handshake::HandshakePacket{backend.protocol, std::string{virtualHost}, virtualPort,
                                         protocol::handshake::ClientIntent::Login}
        .write(frame_);
    if (!protocol::packetFraming::writeUncompressed(output_, frame_.readable()))
        return std::unexpected(BackendLoginError::kLimit);
    frame_.clear();
    protocol::varInt::write(frame_, protocol::login::LoginStartPacket::kId);
    protocol::login::LoginStartPacket{profile.name, profile.id}.write(frame_);
    if (!protocol::packetFraming::writeUncompressed(output_, frame_.readable()))
        return std::unexpected(BackendLoginError::kLimit);
    frame_.clear();
    backendConfig_ = &backend;
    profile_ = &profile;
    remoteAddress_ = remoteAddress;
    remotePort_ = remotePort;
    native_ = native;
    virtualHost_ = virtualHostText;
    virtualPort_ = virtualPort;
    phase_ = Phase::kLogin;
    return {};
}

BackendLogin::Clock::time_point BackendLogin::deadline(bool readingPaused) const {
    if (phase_ == Phase::kRetired)
        return Clock::time_point::max();
    return std::min({preparationDeadline_, writeDeadline_, readingPaused ? Clock::time_point::max() : readDeadline_});
}

void BackendLogin::expectWriteUntil(Clock::time_point deadline) {
    if (hasOutput())
        writeDeadline_ = std::min(writeDeadline_, deadline);
}

void BackendLogin::expectReadUntil(Clock::time_point deadline) { readDeadline_ = deadline; }

void BackendLogin::boundPreparation(Clock::time_point deadline) {
    if (phase_ == Phase::kLogin)
        preparationDeadline_ = std::min(preparationDeadline_, deadline);
}

void BackendLogin::retire() {
    connection_.close();
    cookieKey_ = wire::io::ByteBuffer{0, memory_};
    input_ = wire::io::ByteBuffer{0, memory_};
    output_ = wire::io::ByteBuffer{0, memory_};
    frame_ = wire::io::ByteBuffer{0, memory_};
    phase_ = Phase::kRetired;
    preparationDeadline_ = Clock::time_point::max();
    readDeadline_ = Clock::time_point::max();
    writeDeadline_ = Clock::time_point::max();
}

bool BackendLogin::inLogin() const { return phase_ == Phase::kLogin || phase_ == Phase::kLoginAccepted; }

bool BackendLogin::advance(Phase from, Phase to) {
    if (phase_ != from)
        return false;
    phase_ = to;
    return true;
}

bool BackendLogin::acknowledgeLogin() {
    if (!advance(Phase::kLoginAccepted, Phase::kConfiguration))
        return false;
    backendConfig_ = nullptr;
    profile_ = nullptr;
    remoteAddress_ = {};
    return true;
}

bool BackendLogin::beginConfiguration() { return advance(Phase::kPlay, Phase::kConfiguration); }

bool BackendLogin::finishConfiguration() {
    if (!advance(Phase::kConfiguration, Phase::kPlay))
        return false;
    preparationDeadline_ = Clock::time_point::max();
    return true;
}

std::size_t BackendLogin::allocated() const {
    return cookieKey_.capacity() + input_.capacity() + output_.capacity() + frame_.capacity();
}

bool BackendLogin::bufferedPacket() const {
    std::uint32_t length = 0;
    for (std::size_t i = 0; i < std::min<std::size_t>(3, input_.readableBytes()); ++i) {
        const auto byte = input_.peekByte(i);
        length |= static_cast<std::uint32_t>(byte & 0x7fU) << (7 * i);
        if ((byte & 0x80U) == 0)
            return input_.readableBytes() - i - 1 >= length;
    }
    return false;
}

wire::io::IoInterest BackendLogin::interest(bool read) const {
    if (phase_ == Phase::kRetired)
        return {false, false};
    read = read && !hasCookieRequest();
    return {read, hasOutput() || (read && bufferedPacket())};
}

std::expected<void, BackendLoginError> BackendLogin::queue(std::span<const std::uint8_t> packet,
                                                           std::size_t loginLimit) {
    if (phase_ == Phase::kUnstarted || phase_ == Phase::kRetired || packet.empty())
        return std::unexpected(BackendLoginError::kMalformed);
    output_.discardReadBytes();
    const auto maximum = compression_ ? wire::protocol::packetFraming::kMaxUncompressedSize
                                      : wire::protocol::packetFraming::kMaxFrameSize;
    if (packet.size() > static_cast<std::size_t>(maximum))
        return std::unexpected(BackendLoginError::kLimit);
    const auto room = packet.size() + packet.size() / 1000 + 64;
    if ((inLogin() && (output_.writerIndex() > loginLimit || room > loginLimit - output_.writerIndex())) ||
        !output_.tryReserve(room))
        return std::unexpected(BackendLoginError::kLimit);
    auto written = compression_ ? wire::protocol::packetFraming::writeCompressed(output_, packet, *compression_)
                                : wire::protocol::packetFraming::writeUncompressed(output_, packet);
    if (!written)
        return std::unexpected(BackendLoginError::kLimit);
    return {};
}

std::expected<std::span<const std::uint8_t>, BackendLoginError> BackendLogin::ready(const wire::io::IoReady& event,
                                                                                    bool readEnabled) {
    if (phase_ == Phase::kUnstarted || phase_ == Phase::kRetired)
        return std::unexpected(BackendLoginError::kMalformed);
    if (output_.readableBytes() != 0 && event.writable) {
        auto written = connection_.writeSome(output_.readable());
        if (!written && written.error().kind != wire::io::SocketError::Kind::WouldBlock)
            return std::unexpected(BackendLoginError::kIo);
        if (written)
            output_.skip(*written);
        if (output_.readableBytes() == 0) {
            output_.clear();
            writeDeadline_ = Clock::time_point::max();
        }
    }
    if (hasCookieRequest() && event.closed)
        return std::unexpected(BackendLoginError::kIo);
    if (!readEnabled || hasCookieRequest())
        return std::span<const std::uint8_t>{};
    if (!bufferedPacket() && (event.readable || event.closed)) {
        std::array<std::uint8_t, kCapacity> chunk{};
        const auto room = input_.capacity() - input_.writerIndex();
        if (room == 0)
            return std::unexpected(BackendLoginError::kLimit);
        auto read = connection_.read(std::span{chunk}.first(std::min(room, chunk.size())));
        if (!read && read.error().kind != wire::io::SocketError::Kind::WouldBlock)
            return std::unexpected(BackendLoginError::kIo);
        if (read && *read == 0)
            return std::unexpected(BackendLoginError::kIo);
        if (read)
            input_.writeBytes(std::span{chunk}.first(*read));
    }
    // Consume compression control before decoding any coalesced following frame.
    while (true) {
        const auto hint = wire::protocol::packetFraming::inspectFrame(
            input_.readable(), compression_.has_value(),
            inLogin() ? kLoginPacketLimit : wire::protocol::packetFraming::kMaxUncompressedSize, compression_);
        if (!hint)
            return std::unexpected(hint.error() == wire::protocol::FramingError::FrameTooBig
                                       ? BackendLoginError::kLimit
                                       : BackendLoginError::kMalformed);
        if (!*hint)
            return std::span<const std::uint8_t>{};
        const auto sizes = **hint;
        if (inLogin() && sizes.frameBytes > kLoginPacketLimit + 8)
            return std::unexpected(BackendLoginError::kLimit);
        if (sizes.frameBytes > input_.capacity() && !input_.tryReserve(sizes.frameBytes - input_.writerIndex()))
            return std::unexpected(BackendLoginError::kLimit);
        if (!sizes.complete)
            return std::span<const std::uint8_t>{};
        frame_.clear();
        if (!sizes.packetBytes || (*sizes.packetBytes > frame_.capacity() && !frame_.tryReserve(*sizes.packetBytes)))
            return std::unexpected(BackendLoginError::kLimit);
        const auto packet =
            wire::protocol::packetFraming::tryReadFrame(input_, compression_.has_value(), frame_, compression_);
        if (!packet)
            return std::unexpected(BackendLoginError::kMalformed);
        input_.discardReadBytes();
        if (!*packet)
            return std::span<const std::uint8_t>{};
        const auto body = frame_.readable();
        const auto id = wire::protocol::varInt::read(frame_);
        if (frame_.failed())
            return std::unexpected(BackendLoginError::kMalformed);
        if (!inLogin() || id != wire::protocol::login::LoginCompressionPacket::kId) {
            return body;
        }
        auto compression = wire::protocol::login::LoginCompressionPacket::read(frame_);
        if (!compression || frame_.readableBytes() != 0 || compressionNegotiated_)
            return std::unexpected(BackendLoginError::kMalformed);
        compressionNegotiated_ = true;
        if (compression->threshold >= 0)
            compression_ = compression->threshold;
        else
            compression_.reset();
    }
}

} // namespace basalt::proxy::runtime::detail
