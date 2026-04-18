#include "ClientSession.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/handshake/HandshakePacket.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace basalt::proxy::runtime::detail {

ClientSession::ClientSession(wire::io::TcpConnection connection, wire::io::ResourceBudget::Reservation slot,
                             wire::io::ResourceBudget::Reservation buffers, const config::RuntimeConfig& settings,
                             std::span<const std::uint8_t> status, std::span<const std::uint8_t> unavailable,
                             Clock::time_point acceptedAt)
    : slot_(std::move(slot)), buffers_(std::move(buffers)), connection_(std::move(connection)), settings_(settings),
      status_(status), unavailable_(unavailable),
      readDeadline_(acceptedAt + std::chrono::milliseconds{settings.handshakeTimeoutMs}) {}

bool ClientSession::accounted() const {
    return input_.capacity() + frame_.capacity() + output_.capacity() <= buffers_.size();
}

ClientSession::Clock::time_point ClientSession::deadline() const {
    return closing_ ? writeDeadline_ : std::min(readDeadline_, writeDeadline_);
}

wire::io::IoInterest ClientSession::interest() const { return {!closing_ && !paused_, output_.readableBytes() != 0}; }

bool ClientSession::queue(std::span<const std::uint8_t> bytes) {
    output_.discardReadBytes();
    if (bytes.size() > kBufferCapacity - output_.writerIndex())
        return false;
    if (output_.readableBytes() == 0)
        writeDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs};
    output_.writeBytes(bytes);
    if (output_.readableBytes() >= settings_.highWaterBytes)
        paused_ = true;
    return true;
}

bool ClientSession::packet() {
    namespace protocol = wire::protocol;
    const auto id = protocol::varInt::read(frame_);
    if (frame_.failed())
        return false;
    if (state_ == State::Handshake) {
        if (id != protocol::handshake::HandshakePacket::kId)
            return false;
        const auto hello = protocol::handshake::HandshakePacket::read(frame_);
        if (!hello || frame_.readableBytes() != 0 || hello->hostName.empty() ||
            hello->hostName.find('\0') != std::string::npos)
            return false;
        if (hello->intent == protocol::handshake::ClientIntent::Status) {
            state_ = State::StatusRequest;
        } else if (hello->intent == protocol::handshake::ClientIntent::Login &&
                   hello->protocolVersion == buildInfo::kProtocolVersion) {
            closing_ = true;
            return queue(unavailable_);
        } else {
            return false;
        }
    } else if (state_ == State::StatusRequest) {
        if (id != protocol::status::kStatusRequestId || frame_.readableBytes() != 0 || !queue(status_))
            return false;
        state_ = State::Ping;
    } else {
        if (id != protocol::status::PingPacket::kId)
            return false;
        const auto ping = protocol::status::PingPacket::read(frame_);
        if (!ping || frame_.readableBytes() != 0)
            return false;
        frame_.clear();
        protocol::varInt::write(frame_, protocol::status::PingPacket::kId);
        ping->write(frame_);
        // An opaque eight-byte pong, framed without allocating a staging buffer.
        std::array<std::uint8_t, 10> pong{};
        pong[0] = 9;
        std::ranges::copy(frame_.readable(), pong.begin() + 1);
        closing_ = true;
        return queue(pong);
    }
    readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
    return true;
}

bool ClientSession::decode() {
    while (input_.readableBytes() != 0 && !closing_) {
        // Reject the declared bootstrap size before buffering its body or running a codec.
        std::uint32_t length = 0;
        bool complete = false;
        for (std::size_t i = 0; i < std::min<std::size_t>(input_.readableBytes(), 3); ++i) {
            const auto byte = input_.peekByte(i);
            length |= static_cast<std::uint32_t>(byte & 0x7fU) << (i * 7);
            if ((byte & 0x80U) == 0) {
                complete = true;
                break;
            }
        }
        if (!complete) {
            if (input_.readableBytes() >= 3)
                return false;
            break;
        }
        if (length == 0 || length > kBufferCapacity - 3)
            return false;
        auto decoded = wire::protocol::packetFraming::tryReadFrame(input_, false, frame_);
        if (!decoded)
            return false;
        if (!*decoded)
            break;
        if (!packet())
            return false;
    }
    input_.discardReadBytes();
    return true;
}

bool ClientSession::ready(const wire::io::IoReady& event) {
    if (Clock::now() >= deadline())
        return false;
    if (!closing_ && !paused_ && (event.readable || event.closed)) {
        std::array<std::uint8_t, kBufferCapacity> chunk{};
        const auto room = kBufferCapacity - input_.writerIndex();
        if (room == 0)
            return false;
        const auto read = connection_.read(std::span{chunk}.first(room));
        if (!read) {
            if (read.error().kind != wire::io::SocketError::Kind::WouldBlock)
                return false;
        } else if (*read == 0) {
            closing_ = true;
        } else {
            input_.writeBytes(std::span{chunk}.first(*read));
            if (!decode())
                return false;
        }
    }
    if (output_.readableBytes() != 0) {
        const auto sent = connection_.writeSome(output_.readable());
        if (!sent) {
            if (sent.error().kind != wire::io::SocketError::Kind::WouldBlock)
                return false;
        } else {
            output_.skip(*sent);
            if (output_.readableBytes() == 0) {
                output_.clear();
                writeDeadline_ = Clock::time_point::max();
            }
            if (output_.readableBytes() <= settings_.lowWaterBytes)
                paused_ = false;
        }
    }
    return accounted() && (!closing_ || output_.readableBytes() != 0);
}

} // namespace basalt::proxy::runtime::detail
