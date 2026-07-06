#include "StatusProbe.hpp"

#include <basalt/wire/io/ByteBuffer.hpp>
#include <basalt/wire/io/TcpSocket.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/handshake/HandshakePacket.hpp>

#include <array>
#include <thread>

namespace basalt::proxy::runtime::detail {
namespace {
using Clock = std::chrono::steady_clock;
namespace protocol = wire::protocol;

constexpr std::size_t kMaxResponseBytes = 256 * 1024;
constexpr auto kPollStep = std::chrono::milliseconds{5};

std::optional<wire::io::TcpConnection> connectBefore(const ProbeTarget& target, Clock::time_point deadline,
                                                     const std::stop_token& stop) {
    auto connection = wire::io::TcpConnection::connectNonBlocking(target.host, target.port);
    if (!connection)
        return std::nullopt;
    while (!stop.stop_requested()) {
        const auto done = connection->finishConnect();
        if (!done)
            return std::nullopt;
        if (*done)
            return std::move(*connection);
        if (Clock::now() >= deadline)
            return std::nullopt;
        std::this_thread::sleep_for(kPollStep);
    }
    return std::nullopt;
}

bool writeAll(wire::io::TcpConnection& connection, std::span<const std::uint8_t> bytes, Clock::time_point deadline,
              const std::stop_token& stop) {
    while (!bytes.empty() && !stop.stop_requested()) {
        const auto sent = connection.writeSome(bytes);
        if (sent) {
            bytes = bytes.subspan(*sent);
        } else if (sent.error().kind != wire::io::SocketError::Kind::WouldBlock) {
            return false;
        }
        if (!bytes.empty()) {
            if (Clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(kPollStep);
        }
    }
    return bytes.empty();
}
} // namespace

bool tcpReachable(const ProbeTarget& target, std::chrono::milliseconds timeout, const std::stop_token& stop) {
    return connectBefore(target, Clock::now() + timeout, stop).has_value();
}

std::optional<protocol::status::ServerStatusPayload>
statusPing(const ProbeTarget& target, std::chrono::milliseconds timeout, const std::stop_token& stop) {
    const auto deadline = Clock::now() + timeout;
    auto connection = connectBefore(target, deadline, stop);
    if (!connection)
        return std::nullopt;
    wire::io::ByteBuffer body{256}, request{256};
    protocol::varInt::write(body, protocol::handshake::HandshakePacket::kId);
    protocol::handshake::HandshakePacket{target.protocol, target.host, target.port,
                                         protocol::handshake::ClientIntent::Status}
        .write(body);
    if (!protocol::packetFraming::writeUncompressed(request, body.readable()))
        return std::nullopt;
    body.clear();
    protocol::varInt::write(body, protocol::status::kStatusRequestId);
    if (!protocol::packetFraming::writeUncompressed(request, body.readable()) ||
        !writeAll(*connection, request.readable(), deadline, stop))
        return std::nullopt;

    wire::io::ByteBuffer received{4096}, frame{4096};
    std::array<std::uint8_t, 4096> chunk;
    while (!stop.stop_requested() && Clock::now() < deadline) {
        const auto count = connection->read(chunk);
        if (count && *count == 0)
            return std::nullopt;
        if (!count && count.error().kind != wire::io::SocketError::Kind::WouldBlock)
            return std::nullopt;
        if (count) {
            received.writeBytes(std::span{chunk}.first(*count));
            if (received.readableBytes() > kMaxResponseBytes)
                return std::nullopt;
            const auto complete = protocol::packetFraming::tryReadFrame(received, false, frame);
            if (!complete)
                return std::nullopt;
            if (*complete) {
                if (protocol::varInt::read(frame) != protocol::status::StatusResponsePacket::kId || frame.failed())
                    return std::nullopt;
                auto response = protocol::status::StatusResponsePacket::read(frame);
                if (!response)
                    return std::nullopt;
                return std::move(response->status);
            }
        } else {
            std::this_thread::sleep_for(kPollStep);
        }
    }
    return std::nullopt;
}

} // namespace basalt::proxy::runtime::detail
