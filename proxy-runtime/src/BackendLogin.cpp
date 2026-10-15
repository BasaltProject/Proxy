#include "BackendLogin.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/handshake/HandshakePacket.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>

#include <algorithm>
#include <array>

namespace basalt::proxy::runtime::detail {

BackendLogin::BackendLogin(wire::io::TcpConnection connection) : connection_(std::move(connection)) {}

std::expected<void, BackendLoginError> BackendLogin::start(const config::BackendConfig& backend,
                                                           const wire::auth::SessionProfile& profile,
                                                           std::string_view virtualHost, std::uint16_t virtualPort) {
    namespace protocol = wire::protocol;
    if (started_ || backend.protocol != buildInfo::kProtocolVersion ||
        backend.forwarding != wire::auth::forwarding::ForwardingMode::None)
        return std::unexpected(BackendLoginError::kUnsupported);
    if (!wire::auth::validLoginName(profile.name) || virtualHost.empty() || virtualHost.size() > 255 ||
        virtualHost.find('\0') != std::string_view::npos)
        return std::unexpected(BackendLoginError::kMalformed);
    protocol::varInt::write(frame_, protocol::handshake::HandshakePacket::kId);
    protocol::handshake::HandshakePacket{backend.protocol, std::string{virtualHost}, virtualPort,
                                         protocol::handshake::ClientIntent::Login}
        .write(frame_);
    if (!protocol::packetFraming::writeUncompressed(output_, frame_.readable()))
        return std::unexpected(BackendLoginError::kLimit);
    frame_.clear();
    protocol::varInt::write(frame_, protocol::login::LoginStartPacket::kId);
    protocol::login::LoginStartPacket{profile.name, profile.id}.write(frame_);
    if (!protocol::packetFraming::writeUncompressed(output_, frame_.readable()) || allocated() > kBytes)
        return std::unexpected(BackendLoginError::kLimit);
    frame_.clear();
    started_ = true;
    return {};
}

std::size_t BackendLogin::allocated() const { return input_.capacity() + output_.capacity() + frame_.capacity(); }

std::expected<std::span<const std::uint8_t>, BackendLoginError> BackendLogin::ready(const wire::io::IoReady& event) {
    if (!started_)
        return std::unexpected(BackendLoginError::kMalformed);
    if (output_.readableBytes() != 0 && event.writable) {
        auto written = connection_.writeSome(output_.readable());
        if (!written && written.error().kind != wire::io::SocketError::Kind::WouldBlock)
            return std::unexpected(BackendLoginError::kIo);
        if (written)
            output_.skip(*written);
        if (output_.readableBytes() == 0)
            output_.clear();
    }
    if (event.readable || event.closed) {
        std::array<std::uint8_t, kCapacity> chunk{};
        const auto room = kCapacity - input_.writerIndex();
        if (room == 0)
            return std::unexpected(BackendLoginError::kLimit);
        auto read = connection_.read(std::span{chunk}.first(room));
        if (!read && read.error().kind != wire::io::SocketError::Kind::WouldBlock)
            return std::unexpected(BackendLoginError::kIo);
        if (read && *read == 0)
            return std::unexpected(BackendLoginError::kIo);
        if (read)
            input_.writeBytes(std::span{chunk}.first(*read));
    }
    std::uint32_t length = 0;
    bool complete = false;
    for (std::size_t i = 0; i < std::min<std::size_t>(3, input_.readableBytes()); ++i) {
        const auto byte = input_.peekByte(i);
        length |= static_cast<std::uint32_t>(byte & 0x7fU) << (7 * i);
        if ((byte & 0x80U) == 0) {
            complete = true;
            break;
        }
    }
    if ((!complete && input_.readableBytes() >= 3) || (complete && (length == 0 || length > kCapacity - 3)))
        return std::unexpected(BackendLoginError::kLimit);
    const auto packet = wire::protocol::packetFraming::tryReadFrame(input_, false, frame_);
    if (!packet)
        return std::unexpected(BackendLoginError::kMalformed);
    input_.discardReadBytes();
    if (!*packet)
        return std::span<const std::uint8_t>{};
    return frame_.readable();
}

} // namespace basalt::proxy::runtime::detail
