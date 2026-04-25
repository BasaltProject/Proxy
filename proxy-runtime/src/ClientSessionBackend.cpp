#include "BackendDial.hpp"
#include "BackendLogin.hpp"
#include "ClientSession.hpp"
#include "LoginShapes.hpp"
#include "RouteVersions.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/multiversion/ProtocolVersions.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/login/LoginPluginPackets.hpp>
#include <basalt/wire/protocol/play/BorderPackets.hpp>
#include <basalt/wire/protocol/play/CommandPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::int32_t kConfigurationDisconnectId = 2;
constexpr std::int32_t kStartConfigurationId = 0x78;
} // namespace

bool ClientSession::answerKnownPacks() {
    frame_.clear();
    wire::protocol::varInt::write(frame_, wire::protocol::configuration::SelectKnownPacksPacket::kServerboundId);
    wire::protocol::configuration::SelectKnownPacksPacket{}.write(frame_);
    if (!toBackend(*backend(), frame_.readable()))
        return false;
    backpressure();
    backend()->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    backend()->expectReadUntil(Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs});
    return true;
}

bool ClientSession::backendReady(const wire::io::IoReady& event) {
    return backendEvent(event) && advancePreparedChange();
}

bool ClientSession::backendEvent(const wire::io::IoReady& event) {
    if (Clock::now() >= deadline())
        return timedOut(Clock::now());
    if (connectionStage_ == ConnectionStage::kConnecting && dial_ && event.handle == dial_->handle())
        return dial();
    if (activeBackend_ && candidateBackend_ && event.handle == candidateBackend_->handle())
        return candidateReady(event);
    if ((state_ != State::kBackendLogin && state_ != State::kCookieResponse && state_ != State::kLoginAck &&
         state_ != State::kConfiguration && state_ != State::kPlay) ||
        !backend() || event.handle != backend()->handle())
        return false;
    const bool closingCookie = state_ == State::kCookieResponse && !activeBackend_ && event.closed;
    if (backendPaused_ && event.closed && backend()->hasOutput() && !closingCookie)
        return false;
    if ((state_ == State::kLoginAck || startingConfiguration_) && event.closed)
        return false;
    auto sourceEvent = event;
    if (closingCookie) {
        backend()->cancelCookieRequest();
        sourceEvent.writable = false;
    }
    const bool readBackend = closingCookie || (!backendPaused_ && !startingConfiguration_ &&
                                               state_ != State::kLoginAck && state_ != State::kCookieResponse);
    auto packet = backend()->ready(sourceEvent, readBackend);
    backpressure();
    if (!packet) {
        if (closingCookie && packet.error() == BackendLoginError::kIo) {
            candidateBackend_.reset();
            backpressure();
            return true;
        }
        if (state_ == State::kPlay && activeBackend_ && packet.error() == BackendLoginError::kIo)
            return recoverBackend();
        if (state_ == State::kBackendLogin && !activeBackend_ && packet.error() == BackendLoginError::kIo) {
            candidateBackend_.reset();
            return route();
        }
        return false;
    }
    if (packet->empty())
        return true;
    const bool relay = state_ == State::kConfiguration || state_ == State::kPlay;
    if (!relay && packet->size() > BackendLogin::kLoginPacketLimit)
        return false;
    std::span<const std::uint8_t> bytes = *packet;
    if (relay && numbered(protocolState())) {
        if (ids_->apply(IdRenumbering::Flow::kBackendToSession, protocolState(), bytes, idIn_) !=
            IdRenumbering::Outcome::kRenumbered)
            return false;
        bytes = idIn_.readable();
    }
    return backendFrame(bytes);
}

bool ClientSession::backendFrame(std::span<const std::uint8_t> bytes) {
    frame_.clear();
    if (!frame_.tryReserve(bytes.size()))
        return false;
    frame_.writeBytes(bytes);
    const auto id = wire::protocol::varInt::read(frame_);
    if (frame_.failed())
        return false;
    if (state_ == State::kLoginAck ||
        (state_ == State::kCookieResponse && id != wire::protocol::login::LoginDisconnectPacket::kId))
        return false;
    if (id == wire::protocol::cookie::RequestPacket::kLoginId && state_ == State::kBackendLogin) {
        auto action = backend()->requestCookie(frame_, cookieCache_);
        if (!action)
            return false;
        if (*action == BackendLogin::CookieAction::kCached) {
            backend()->expectWriteUntil(
                std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs}));
            backpressure();
            return true;
        }
        return publishCookieRequest(*backend()) == CookiePublication::kPublished;
    }
    if (state_ == State::kConfiguration || state_ == State::kPlay) {
        if (id < 0)
            return false;
        if (id == (state_ == State::kConfiguration ? wire::protocol::configuration::KeepAlivePacket::kId
                                                   : wire::protocol::play::PlayKeepAlivePacket::kClientboundId))
            return backendChallenge();
        if (awaitingJoin_ && state_ == State::kPlay) {
            awaitingJoin_ = false;
            backpressure();
        }
        const bool shaped = !legacyRaw_;
        if (shaped && id == (state_ == State::kConfiguration ? 0x05 : 0x3d))
            return backendChallenge(true);
        const bool transfer =
            shaped && id == (state_ == State::kConfiguration ? 0x0c : wire::protocol::play::TransferPacket::kId);
        const bool terminal = transfer || (state_ == State::kConfiguration && id == kConfigurationDisconnectId) ||
                              (state_ == State::kPlay && id == wire::protocol::play::PlayDisconnectPacket::kId);
        if (transfer) {
            if (bytes.size() > 512)
                return false;
            auto destination = wire::protocol::play::TransferPacket::read(frame_);
            if (!destination || frame_.readableBytes() != 0 || destination->host.empty() ||
                destination->host.size() > 255 || destination->port < 1 || destination->port > 65535 ||
                std::ranges::any_of(destination->host, [](unsigned char c) { return c <= 32 || c == 127; }))
                return false;
        } else if (terminal) {
            if (shaped && (!wire::protocol::play::PlayDisconnectPacket::read(frame_) || frame_.readableBytes() != 0))
                return false;
        } else if (shaped && state_ == State::kPlay && id == kStartConfigurationId) {
            if (startingConfiguration_ || frame_.readableBytes() != 0)
                return false;
            if (candidateBackend_ || connectionStage_ != ConnectionStage::kIdle)
                discardCandidate();
            startingConfiguration_ = true;
        } else if (state_ == State::kConfiguration && id == wire::protocol::configuration::kFinishConfigurationId) {
            if (finishingConfiguration_ || frame_.readableBytes() != 0)
                return false;
            finishingConfiguration_ = true;
        } else if (state_ == State::kConfiguration && finishingConfiguration_) {
            return false;
        }
        if (state_ == State::kConfiguration && translation_ &&
            clientProtocol_ < wire::multiversion::protocolVersions::kV1_20_5 &&
            id == wire::protocol::configuration::SelectKnownPacksPacket::kClientboundId)
            return answerKnownPacks();
        if (shaped && state_ == State::kPlay && playerCommandNames_ &&
            id == wire::protocol::play::CommandsPacket::kId) {
            if (!relayCommandTree(bytes))
                return false;
            backend()->expectReadUntil(Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs});
            return true;
        }
        if (id == (state_ == State::kConfiguration ? 0x01 : 0x18)) {
            const auto result = pluginMessage(true);
            if (result != PluginMessageResult::kRelay) {
                if (result == PluginMessageResult::kInvalid)
                    return false;
                backend()->expectReadUntil(Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs});
                return true;
            }
        }
        const auto packPushId = state_ == State::kConfiguration ? 0x09 : 0x51;
        const auto packPopId = state_ == State::kConfiguration ? 0x08 : 0x50;
        if (shaped && (id == packPushId || id == packPopId))
            return backendResourcePack(id == packPushId);
        const auto cookieRequestId = state_ == State::kConfiguration ? 0x00 : 0x15;
        const auto cookieStoreId = state_ == State::kConfiguration ? 0x0b : 0x7a;
        if (!shaped) {
        } else if (id == cookieRequestId) {
            auto request = wire::protocol::cookie::RequestPacket::read(frame_);
            if (!request || frame_.readableBytes() != 0 ||
                !pendingCookies_.add(request->key, cookieCache_.lookup(request->key).storeRevision,
                                     backend()->attempt()))
                return false;
        } else if (id == cookieStoreId) {
            auto store = wire::protocol::cookie::StorePacket::read(frame_);
            if (!store || frame_.readableBytes() != 0 ||
                !cookieCache_.store(store->key, std::span<const std::uint8_t>{store->payload}))
                return false;
        }
        if (!queuePacket(bytes))
            return false;
        backend()->expectReadUntil(Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs});
        if (legacyClient() && state_ == State::kConfiguration && finishingConfiguration_ &&
            id == wire::protocol::configuration::kFinishConfigurationId)
            return synthesizeClientPacket(wire::protocol::configuration::kFinishConfigurationId);
        if (terminal) {
            closing_ = true;
            closeBackends();
        }
        return true;
    }
    if (id == wire::protocol::login::LoginFinishedPacket::kId) {
        if (!profile_)
            return false;
        auto finished = backend()->acceptLogin(frame_);
        if (!finished)
            return false;
        frame_.clear();
        if (compressionThreshold_ >= 0) {
            wire::protocol::varInt::write(frame_, wire::protocol::login::LoginCompressionPacket::kId);
            wire::protocol::login::LoginCompressionPacket{compressionThreshold_}.write(frame_);
            if (!queuePacket(frame_.readable()))
                return false;
            compression_ = compressionThreshold_;
            frame_.clear();
        }
        wire::protocol::varInt::write(frame_, wire::protocol::login::LoginFinishedPacket::kId);
        if (clientProtocol_ != 0 && clientProtocol_ < wire::multiversion::protocolVersions::kV1_20_2) {
            writeLoginSuccess(frame_, clientProtocol_, *finished);
            if (!queueNative(frame_.readable()))
                return false;
            state_ = State::kLoginAck;
            return synthesizeClientPacket(wire::protocol::login::kLoginAcknowledgedId);
        }
        finished->write(frame_);
        auto encoded = frame_.readable();
        std::vector<std::uint8_t> older;
        if (ids_ && !translation_ && loginFinishedTail(clientProtocol_) != LoginFinishedTail::kSessionId) {
            older.assign(encoded.begin(), encoded.end() - 16);
            if (loginFinishedTail(clientProtocol_) == LoginFinishedTail::kStrictErrorsFlag)
                older.push_back(1);
            encoded = older;
        }
        if (!queuePacket(encoded))
            return false;
        state_ = State::kLoginAck;
        if (legacyClient())
            return synthesizeClientPacket(wire::protocol::login::kLoginAcknowledgedId);
        return true;
    }
    if (id == wire::protocol::login::LoginPluginRequestPacket::kId) {
        if (!backend()->answerPluginQuery(frame_))
            return false;
        backpressure();
        backend()->expectWriteUntil(
            std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs}));
        return true;
    }
    closing_ = true;
    if (id != wire::protocol::login::LoginDisconnectPacket::kId) {
        closeBackends();
        return queue(unavailable_);
    }
    const auto reason = wire::protocol::readString(frame_);
    if (frame_.failed() || frame_.readableBytes() != 0 || !wire::common::parseJson(reason))
        return false;
    frame_.clear();
    if (!wire::protocol::packetFraming::writeUncompressed(frame_, bytes))
        return false;
    closeBackends();
    return queue(frame_.readable());
}

} // namespace basalt::proxy::runtime::detail
