#include "BackendLogin.hpp"
#include "ClientSession.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/auth/forwarding/VelocityForwarding.hpp>
#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/login/LoginPluginPackets.hpp>
#include <basalt/wire/protocol/play/BorderPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::int32_t kConfigurationDisconnectId = 2;
constexpr std::int32_t kStartConfigurationId = 0x78;
} // namespace

bool ClientSession::backendReady(const wire::io::IoReady& event) {
    if (Clock::now() >= deadline())
        return false;
    if (state_ == State::kConnecting && dial_)
        return dial();
    if ((state_ != State::kBackendLogin && state_ != State::kCookieResponse && state_ != State::kLoginAck &&
         state_ != State::kConfiguration && state_ != State::kPlay) ||
        !backend())
        return false;
    if (backendPaused_ && event.closed && backend()->hasOutput())
        return false;
    if ((state_ == State::kCookieResponse || state_ == State::kLoginAck || startingConfiguration_) && event.closed)
        return false;
    auto packet = backend()->ready(event, !backendPaused_ && !startingConfiguration_ && state_ != State::kLoginAck &&
                                              state_ != State::kCookieResponse);
    backpressure();
    if (!packet)
        return false;
    if (!backend()->hasOutput())
        backendWriteDeadline_ = Clock::time_point::max();
    if (packet->empty())
        return true;
    const bool relay = state_ == State::kConfiguration || state_ == State::kPlay;
    if (!relay && packet->size() > BackendLogin::kLoginPacketLimit)
        return false;
    frame_.clear();
    if (!frame_.tryReserve(packet->size()))
        return false;
    frame_.writeBytes(*packet);
    const auto id = wire::protocol::varInt::read(frame_);
    if (frame_.failed())
        return false;
    if (state_ == State::kLoginAck)
        return false;
    if (id == wire::protocol::cookie::RequestPacket::kLoginId && state_ == State::kBackendLogin) {
        auto request = wire::protocol::cookie::RequestPacket::read(frame_);
        if (!request || frame_.readableBytes() != 0 || request->key.size() > 256 || cookie_)
            return false;
        const auto cached = cookieCache_.lookup(request->key);
        if (cached.known) {
            frame_.clear();
            if (!frame_.tryReserve(wire::protocol::cookie::kMaxPayloadBytes + 512))
                return false;
            wire::protocol::varInt::write(frame_, wire::protocol::cookie::ResponsePacket::kLoginId);
            wire::protocol::writeString(frame_, request->key);
            wire::protocol::writeBoolean(frame_, cached.payload.has_value());
            if (cached.payload)
                wire::protocol::writeByteArray(frame_, *cached.payload);
            if (!backend()->queue(frame_.readable(), wire::protocol::cookie::kMaxPayloadBytes + 512))
                return false;
            if (backendWriteDeadline_ == Clock::time_point::max())
                backendWriteDeadline_ =
                    std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
            backpressure();
            return true;
        }
        cookie_ = std::move(request->key);
        if (!queuePacket(*packet))
            return false;
        state_ = State::kCookieResponse;
        return true;
    }
    if (state_ == State::kConfiguration || state_ == State::kPlay) {
        if (id < 0)
            return false;
        const bool transfer =
            id == (state_ == State::kConfiguration ? 0x0c : wire::protocol::play::TransferPacket::kId);
        const bool terminal = transfer || (state_ == State::kConfiguration && id == kConfigurationDisconnectId) ||
                              (state_ == State::kPlay && id == wire::protocol::play::PlayDisconnectPacket::kId);
        if (transfer) {
            if (packet->size() > 512)
                return false;
            auto destination = wire::protocol::play::TransferPacket::read(frame_);
            if (!destination || frame_.readableBytes() != 0 || destination->host.empty() ||
                destination->host.size() > 255 || destination->port < 1 || destination->port > 65535 ||
                std::ranges::any_of(destination->host, [](unsigned char c) { return c <= 32 || c == 127; }))
                return false;
        } else if (terminal) {
            if (!wire::protocol::play::PlayDisconnectPacket::read(frame_) || frame_.readableBytes() != 0)
                return false;
        } else if (state_ == State::kPlay && id == kStartConfigurationId) {
            if (startingConfiguration_ || frame_.readableBytes() != 0)
                return false;
            startingConfiguration_ = true;
        } else if (state_ == State::kConfiguration && id == wire::protocol::configuration::kFinishConfigurationId) {
            if (finishingConfiguration_ || frame_.readableBytes() != 0)
                return false;
            finishingConfiguration_ = true;
        } else if (state_ == State::kConfiguration && finishingConfiguration_) {
            return false;
        }
        const auto cookieRequestId = state_ == State::kConfiguration ? 0x00 : 0x15;
        const auto cookieStoreId = state_ == State::kConfiguration ? 0x0b : 0x7a;
        if (id == cookieRequestId) {
            auto request = wire::protocol::cookie::RequestPacket::read(frame_);
            if (!request || frame_.readableBytes() != 0 ||
                !pendingCookies_.add(request->key, cookieCache_.lookup(request->key).storeRevision))
                return false;
        } else if (id == cookieStoreId) {
            auto store = wire::protocol::cookie::StorePacket::read(frame_);
            if (!store || frame_.readableBytes() != 0 ||
                !cookieCache_.store(store->key, std::span<const std::uint8_t>{store->payload}))
                return false;
        }
        if (!queuePacket(*packet))
            return false;
        if (terminal) {
            closing_ = true;
            closeBackends();
        }
        backendReadDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
        return true;
    }
    if (id == wire::protocol::login::LoginFinishedPacket::kId) {
        const auto mode = backends_[targets_[nextTarget_ - 1]].forwarding;
        if (mode == wire::auth::forwarding::ForwardingMode::Velocity && !velocityForwarded_)
            return false;
        // Bound collection allocation before the shared decoder materializes properties.
        (void)wire::protocol::readUuid(frame_);
        (void)wire::protocol::readString(frame_, 16);
        const auto propertyCount = wire::protocol::varInt::read(frame_);
        if (frame_.failed() || propertyCount < 0 || propertyCount > 64)
            return false;
        frame_.clear();
        frame_.writeBytes(*packet);
        (void)wire::protocol::varInt::read(frame_);
        auto finished = wire::protocol::login::LoginFinishedPacket::read(frame_);
        if (!finished || frame_.readableBytes() != 0 || !profile_ || finished->gameProfile.name != profile_->name ||
            (finished->gameProfile.id != profile_->id &&
             (mode != wire::auth::forwarding::ForwardingMode::None ||
              finished->gameProfile.id != wire::common::Uuid::offlinePlayer(profile_->name))))
            return false;
        // Tokens are backend credentials and must never be exposed as client profile properties.
        std::string_view tokenProperty;
        if (mode == wire::auth::forwarding::ForwardingMode::BungeeGuard)
            tokenProperty = wire::auth::forwarding::ProxyForwarding::kBungeeGuardProperty;
        else if (mode == wire::auth::forwarding::ForwardingMode::SafeNet)
            tokenProperty = backends_[targets_[nextTarget_ - 1]].propertyName;
        if (!tokenProperty.empty())
            std::erase_if(finished->gameProfile.properties,
                          [tokenProperty](const auto& entry) { return entry.name == tokenProperty; });
        // The backend's explicit offline identity must not replace the verified frontend UUID.
        finished->gameProfile.id = profile_->id;
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
        finished->write(frame_);
        if (!queuePacket(frame_.readable()))
            return false;
        state_ = State::kLoginAck;
        return true;
    }
    if (id == wire::protocol::login::LoginPluginRequestPacket::kId) {
        auto request = wire::protocol::login::LoginPluginRequestPacket::read(frame_);
        if (!request || frame_.readableBytes() != 0)
            return false;
        namespace velocity = wire::auth::forwarding::velocity;
        const auto& target = backends_[targets_[nextTarget_ - 1]];
        if (target.forwarding == wire::auth::forwarding::ForwardingMode::Velocity &&
            request->channel == velocity::kChannel) {
            if (velocityForwarded_ || !profile_ || request->payload.size() > 1)
                return false;
            const auto requested = request->payload.empty() ? 1 : request->payload[0];
            if (requested == 0)
                return false;
            const auto version = requested >= 4 ? 4 : 1;
            frame_.clear();
            if (!frame_.tryReserve(kAuthenticationBytes))
                return false;
            wire::protocol::varInt::write(frame_, wire::protocol::login::LoginPluginResponsePacket::kId);
            wire::protocol::varInt::write(frame_, request->transactionId);
            wire::protocol::writeBoolean(frame_, true);
            if (!velocity::signInto(frame_, target.secret, connection_.remoteAddress(), profile_->id, profile_->name,
                                    profile_->properties, version) ||
                !backend()->queue(frame_.readable(), kAuthenticationBytes))
                return false;
            velocityForwarded_ = true;
            backpressure();
            if (backendWriteDeadline_ == Clock::time_point::max())
                backendWriteDeadline_ =
                    std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
            return true;
        }
        frame_.clear();
        wire::protocol::varInt::write(frame_, wire::protocol::login::LoginPluginResponsePacket::kId);
        wire::protocol::login::LoginPluginResponsePacket{request->transactionId, std::nullopt}.write(frame_);
        if (!backend()->queue(frame_.readable()))
            return false;
        if (backendWriteDeadline_ == Clock::time_point::max())
            backendWriteDeadline_ =
                std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
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
    if (!wire::protocol::packetFraming::writeUncompressed(frame_, *packet))
        return false;
    closeBackends();
    return queue(frame_.readable());
}

} // namespace basalt::proxy::runtime::detail
