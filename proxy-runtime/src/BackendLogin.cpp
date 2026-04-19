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

wire::io::IoInterest BackendLogin::interest() const { return {true, hasOutput() || bufferedPacket()}; }

std::expected<void, BackendLoginError> BackendLogin::queue(std::span<const std::uint8_t> packet) {
    if (!started_ || packet.empty())
        return std::unexpected(BackendLoginError::kMalformed);
    output_.discardReadBytes();
    // Covers zlib's worst-case expansion and both VarInt headers without buffer growth.
    if (packet.size() > kCapacity - 64 || packet.size() + 64 > kCapacity - output_.writerIndex())
        return std::unexpected(BackendLoginError::kLimit);
    auto written = compression_ ? wire::protocol::packetFraming::writeCompressed(output_, packet, *compression_)
                                : wire::protocol::packetFraming::writeUncompressed(output_, packet);
    if (!written || allocated() > kBytes)
        return std::unexpected(BackendLoginError::kLimit);
    return {};
}

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
    if (!bufferedPacket() && (event.readable || event.closed)) {
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
    // Consume compression control before decoding any coalesced following frame.
    while (true) {
        std::uint32_t length = 0;
        std::size_t headerBytes = 0;
        for (std::size_t i = 0; i < std::min<std::size_t>(3, input_.readableBytes()); ++i) {
            const auto byte = input_.peekByte(i);
            length |= static_cast<std::uint32_t>(byte & 0x7fU) << (7 * i);
            if ((byte & 0x80U) == 0) {
                headerBytes = i + 1;
                break;
            }
        }
        if ((headerBytes == 0 && input_.readableBytes() >= 3) ||
            (headerBytes != 0 && (length == 0 || length > kCapacity - 3)))
            return std::unexpected(BackendLoginError::kLimit);
        if (headerBytes == 0)
            return std::span<const std::uint8_t>{};
        if (compression_) {
            std::uint32_t expanded = 0;
            bool complete = false;
            const auto available = std::min<std::size_t>(length, input_.readableBytes() - headerBytes);
            for (std::size_t i = 0; i < std::min<std::size_t>(5, available); ++i) {
                const auto byte = input_.peekByte(headerBytes + i);
                if (i == 4 && (byte & 0xf0U) != 0)
                    return std::unexpected(BackendLoginError::kMalformed);
                expanded |= static_cast<std::uint32_t>(byte & 0x7fU) << (7 * i);
                if ((byte & 0x80U) == 0) {
                    complete = true;
                    break;
                }
            }
            if ((!complete && (available >= 5 || available == length)))
                return std::unexpected(BackendLoginError::kMalformed);
            if (complete && expanded > kCapacity)
                return std::unexpected(BackendLoginError::kLimit);
        }
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
        if (!login_ || id != wire::protocol::login::LoginCompressionPacket::kId) {
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
