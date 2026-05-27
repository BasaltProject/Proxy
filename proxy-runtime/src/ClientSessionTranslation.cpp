#include "BackendDial.hpp"
#include "BackendLogin.hpp"
#include "BackendRoutes.hpp"
#include "ClientSession.hpp"
#include "LoginShapes.hpp"
#include "NativeForwarding.hpp"
#include "RelayCounters.hpp"
#include "RouteVersions.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/auth/SessionKeyPair.hpp>
#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/multiversion/ProtocolVersions.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>
#include <basalt/wire/protocol/handshake/HandshakePacket.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/play/ChatPackets.hpp>
#include <basalt/wire/protocol/play/CommandPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>
#include <basalt/wire/text/ComponentJson.hpp>
#include <basalt/wire/text/MiniMessage.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace basalt::proxy::runtime::detail {

wire::protocol::ProtocolState ClientSession::protocolState() const {
    using wire::protocol::ProtocolState;
    switch (state_) {
    case State::Handshake:
        return ProtocolState::Handshake;
    case State::StatusRequest:
    case State::Ping:
        return ProtocolState::Status;
    case State::kConfiguration:
        return ProtocolState::Configuration;
    case State::kPlay:
        return ProtocolState::Play;
    default:
        return ProtocolState::Login;
    }
}

bool ClientSession::rawProtocol(std::int32_t protocol) const {
    return ids_ && clientProtocol_ != 0 && protocol == clientProtocol_;
}

bool ClientSession::rawNow() const { return backend() && rawProtocol(backend()->protocol()); }

bool ClientSession::numbered(wire::protocol::ProtocolState state) const {
    return rawNow() && (legacyRaw_ ? state == wire::protocol::ProtocolState::Play : IdRenumbering::applies(state));
}

bool ClientSession::queueLoginPacket(std::span<const std::uint8_t> packet) {
    return rawNow() ? queueNative(packet) : queuePacket(packet);
}

bool ClientSession::keepAliveVarInt() const { return legacyRaw_ && keepAliveIsVarInt(clientProtocol_); }

bool ClientSession::queuePacket(std::span<const std::uint8_t> packet) {
    if (numbered(protocolState())) {
        switch (ids_->apply(IdRenumbering::Flow::kSessionToClient, protocolState(), packet, idOut_)) {
        case IdRenumbering::Outcome::kRenumbered:
            return queueNative(idOut_.readable());
        case IdRenumbering::Outcome::kDropped:
            return true;
        default:
            return false;
        }
    }
    return translation_ ? queueTranslated(packet) : queueNative(packet);
}

bool ClientSession::upgrades(std::int32_t backendProtocol) const {
    return clientProtocol_ == 0 && !translation_ && upwardVerified(backendProtocol);
}

bool ClientSession::upgradeFrame(std::span<const std::uint8_t> frame) {
    namespace protocol = wire::protocol;
    if (!upward_) {
        upward_ = wire::multiversion::java::javaTranslators::upChainFor(backend()->protocol());
        if (!upward_)
            return false;
    }
    wire::io::ByteBuffer view;
    view.writeBytes(frame);
    const auto id = protocol::varInt::read(view);
    if (view.failed())
        return false;
    const std::vector<std::uint8_t> body{view.readable().begin(), view.readable().end()};
    bool ok = true;
    upward_->chain().clientbound(protocolState(), id, body,
                                 [&](std::int32_t upId, std::span<const std::uint8_t> bytes) {
                                     if (!ok)
                                         return;
                                     wire::io::ByteBuffer whole;
                                     protocol::varInt::write(whole, upId);
                                     whole.writeBytes(bytes);
                                     ok = backendFrame(whole.readable());
                                 });
    return ok;
}

bool ClientSession::downgradeFrame(BackendLogin& target, std::span<const std::uint8_t> frame) {
    namespace protocol = wire::protocol;
    if (!upward_) {
        upward_ = wire::multiversion::java::javaTranslators::upChainFor(target.protocol());
        if (!upward_)
            return false;
    }
    const auto phase = target.phase();
    const auto state = phase == BackendLogin::Phase::kPlay            ? protocol::ProtocolState::Play
                       : phase == BackendLogin::Phase::kConfiguration ? protocol::ProtocolState::Configuration
                                                                      : protocol::ProtocolState::Login;
    wire::io::ByteBuffer view;
    view.writeBytes(frame);
    const auto id = protocol::varInt::read(view);
    if (view.failed())
        return false;
    const std::vector<std::uint8_t> body{view.readable().begin(), view.readable().end()};
    bool ok = true;
    upward_->chain().serverbound(state, id, body, [&](std::int32_t downId, std::span<const std::uint8_t> bytes) {
        if (!ok)
            return;
        wire::io::ByteBuffer whole;
        protocol::varInt::write(whole, downId);
        whole.writeBytes(bytes);
        ok = static_cast<bool>(target.queue(whole.readable()));
    });
    return ok;
}

bool ClientSession::toBackend(BackendLogin& target, std::span<const std::uint8_t> frame) {
    if (upgrades(target.protocol()) &&
        (target.phase() == BackendLogin::Phase::kConfiguration || target.phase() == BackendLogin::Phase::kPlay))
        return downgradeFrame(target, frame);
    if (rawProtocol(target.protocol())) {
        const auto phase = target.phase();
        if (legacyRaw_ && (phase == BackendLogin::Phase::kConfiguration ||
                           (phase == BackendLogin::Phase::kLoginAccepted && !frame.empty() &&
                            frame[0] == wire::protocol::login::kLoginAcknowledgedId)))
            return true;
        const auto state = phase == BackendLogin::Phase::kPlay            ? wire::protocol::ProtocolState::Play
                           : phase == BackendLogin::Phase::kConfiguration ? wire::protocol::ProtocolState::Configuration
                                                                          : wire::protocol::ProtocolState::Login;
        if (numbered(state)) {
            switch (ids_->apply(IdRenumbering::Flow::kSessionToBackend, state, frame, idOut_)) {
            case IdRenumbering::Outcome::kRenumbered:
                return static_cast<bool>(target.queue(idOut_.readable()));
            case IdRenumbering::Outcome::kDropped:
                return true;
            default:
                return false;
            }
        }
    }
    return static_cast<bool>(target.queue(frame));
}

std::int32_t ClientSession::speaksTo(const config::BackendConfig& backend, std::int32_t client) const {
    constexpr std::int32_t kFirstWithLoginQueries = 393;
    const bool needsQueries = backend.forwarding == wire::auth::forwarding::ForwardingMode::Velocity ||
                              backend.forwarding == wire::auth::forwarding::ForwardingMode::Basalt;
    if (backend.protocol == 0 &&
        (translationMode_ == config::TranslationMode::Proxy ||
         (backend.automatic && needsQueries && client != 0 && client < kFirstWithLoginQueries)))
        return buildInfo::kProtocolVersion;
    return effectiveProtocol(backend.protocol, client);
}

bool ClientSession::routeAllowed(std::int32_t client, const config::BackendConfig& backend) const {
    const std::int32_t speaks = speaksTo(backend, client);
    if (translationMode_ == config::TranslationMode::Off && client != buildInfo::kProtocolVersion &&
        speaks == buildInfo::kProtocolVersion)
        return false;
    return resolveRoute(client, speaks).has_value();
}

bool ClientSession::acceptsBackend(const config::BackendConfig& backend) const {
    constexpr std::int32_t kNative = buildInfo::kProtocolVersion;
    constexpr std::int32_t kFirstWithLoginQueries = 393;
    const std::int32_t protocol = speaksTo(backend, clientProtocol_);
    using Mode = wire::auth::forwarding::ForwardingMode;
    // The signed schemes ride on a login query, which a client before 1.13 cannot carry.
    if ((backend.forwarding == Mode::Velocity || backend.forwarding == Mode::Basalt) && protocol != kNative &&
        protocol < kFirstWithLoginQueries)
        return false;
    if (translationMode_ == config::TranslationMode::Off && clientProtocol_ != 0 && protocol == kNative)
        return false;
    if (legacyRaw_ || (ids_ && !translation_))
        return protocol == clientProtocol_;
    if (ids_)
        return protocol == clientProtocol_ || protocol == kNative;
    return protocol == kNative || upgrades(protocol);
}

bool ClientSession::legacyClient() const {
    return translation_ && clientProtocol_ < wire::multiversion::protocolVersions::kV1_20_2;
}

bool ClientSession::synthesizeClientPacket(std::int32_t id) {
    frame_.clear();
    wire::protocol::varInt::write(frame_, id);
    synthesizing_ = true;
    const bool ok = packet();
    synthesizing_ = false;
    return ok;
}

bool ClientSession::queueTranslated(std::span<const std::uint8_t> packet) {
    namespace protocol = wire::protocol;
    wire::io::ByteBuffer view;
    view.writeBytes(packet);
    const auto id = protocol::varInt::read(view);
    if (view.failed())
        return false;
    const auto body = view.readable();
    bool ok = true;
    translation_->chain().clientbound(protocolState(), id, body,
                                      [&](std::int32_t translatedId, std::span<const std::uint8_t> bytes) {
                                          if (!ok)
                                              return;
                                          wire::io::ByteBuffer whole;
                                          protocol::varInt::write(whole, translatedId);
                                          whole.writeBytes(bytes);
                                          ok = queueNative(whole.readable());
                                      });
    return ok;
}

bool ClientSession::translateInbound() {
    namespace protocol = wire::protocol;
    const auto id = protocol::varInt::read(frame_);
    if (frame_.failed())
        return false;
    const auto readable = frame_.readable();
    const std::vector<std::uint8_t> body{readable.begin(), readable.end()};
    bool ok = true;
    translation_->chain().serverbound(protocolState(), id, body,
                                      [&](std::int32_t translatedId, std::span<const std::uint8_t> bytes) {
                                          if (!ok)
                                              return;
                                          frame_.clear();
                                          if (!frame_.tryReserve(bytes.size() + 8)) {
                                              ok = false;
                                              return;
                                          }
                                          protocol::varInt::write(frame_, translatedId);
                                          frame_.writeBytes(bytes);
                                          ok = packet();
                                      });
    return ok;
}

bool ClientSession::queueNative(std::span<const std::uint8_t> packet) {
    output_.discardReadBytes();
    const auto maximum = compression_ ? wire::protocol::packetFraming::kMaxUncompressedSize
                                      : wire::protocol::packetFraming::kMaxFrameSize;
    if (packet.empty() || packet.size() > static_cast<std::size_t>(maximum) ||
        !output_.tryReserve(packet.size() + packet.size() / 1000 + 64))
        return false;
    if (output_.readableBytes() == 0)
        writeDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs};
    const auto offset = output_.writerIndex();
    auto encoded = compression_ ? wire::protocol::packetFraming::writeCompressed(output_, packet, *compression_)
                                : wire::protocol::packetFraming::writeUncompressed(output_, packet);
    if (!encoded)
        return false;
    relayCounters().towardClients.fetch_add(output_.writerIndex() - offset, std::memory_order_relaxed);
    if (encryptor_)
        encryptor_->encrypt(output_.mutableSpan(offset));
    if (output_.readableBytes() >= settings_.highWaterBytes)
        backendPaused_ = true;
    return true;
}

} // namespace basalt::proxy::runtime::detail
