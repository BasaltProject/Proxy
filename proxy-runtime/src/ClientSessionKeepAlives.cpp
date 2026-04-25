#include "BackendLogin.hpp"
#include "ClientSession.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>

namespace basalt::proxy::runtime::detail {

bool ClientSession::backendChallenge(bool ping) {
    const auto token = ping                ? frame_.readInt()
                       : keepAliveVarInt() ? wire::protocol::varInt::read(frame_)
                                           : frame_.readLong();
    if (frame_.failed() || frame_.readableBytes() != 0)
        return false;
    const auto published = (ping ? pendingPings_ : pendingKeepAlives_).publish(token, backend()->attempt());
    if (!published)
        return false;
    frame_.clear();
    wire::protocol::varInt::write(frame_, ping ? (state_ == State::kConfiguration ? 0x05 : 0x3d)
                                          : state_ == State::kConfiguration
                                              ? wire::protocol::configuration::KeepAlivePacket::kId
                                              : wire::protocol::play::PlayKeepAlivePacket::kClientboundId);
    if (ping)
        frame_.writeInt(static_cast<std::int32_t>(*published));
    else if (keepAliveVarInt())
        wire::protocol::varInt::write(frame_, static_cast<std::int32_t>(*published));
    else
        frame_.writeLong(*published);
    if (!queuePacket(frame_.readable()))
        return false;
    backend()->expectReadUntil(Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs});
    backpressure();
    return true;
}

bool ClientSession::challengeResponse(bool pong) {
    const auto token = pong                ? frame_.readInt()
                       : keepAliveVarInt() ? wire::protocol::varInt::read(frame_)
                                           : frame_.readLong();
    if (frame_.failed() || frame_.readableBytes() != 0)
        return false;
    const auto origin = (pong ? pendingPings_ : pendingKeepAlives_).consume(token);
    if (!origin || recovering_ || origin->backend != backend()->attempt())
        return true;
    frame_.clear();
    wire::protocol::varInt::write(frame_, pong ? (state_ == State::kConfiguration ? 0x05 : 0x2d)
                                          : state_ == State::kConfiguration
                                              ? wire::protocol::configuration::KeepAlivePacket::kId
                                              : wire::protocol::play::PlayKeepAlivePacket::kServerboundId);
    if (pong)
        frame_.writeInt(static_cast<std::int32_t>(origin->token));
    else if (keepAliveVarInt())
        wire::protocol::varInt::write(frame_, static_cast<std::int32_t>(origin->token));
    else
        frame_.writeLong(origin->token);
    if (!toBackend(*backend(), frame_.readable()))
        return false;
    backend()->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
    backpressure();
    return true;
}

} // namespace basalt::proxy::runtime::detail
