#include "BackendLogin.hpp"
#include "NativeForwarding.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/auth/forwarding/BasaltForwarding.hpp>
#include <basalt/wire/auth/forwarding/VelocityForwarding.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/login/LoginPluginPackets.hpp>

#include <algorithm>

namespace basalt::proxy::runtime::detail {

std::expected<void, BackendLoginError> BackendLogin::answerPluginQuery(wire::io::ByteBuffer& query) {
    namespace protocol = wire::protocol;
    namespace velocity = wire::auth::forwarding::velocity;
    if (phase_ != Phase::kLogin || hasCookieRequest() || query.readableBytes() > kLoginPacketLimit)
        return std::unexpected(BackendLoginError::kMalformed);
    const auto& backend = *backendConfig_;
    const auto& profile = *profile_;
    const auto transaction = protocol::varInt::read(query);
    const auto channelBytes = protocol::varInt::read(query);
    if (query.failed() || channelBytes < 0 || channelBytes > 32767 * 3)
        return std::unexpected(BackendLoginError::kMalformed);
    const auto channel = query.readSpan(static_cast<std::size_t>(channelBytes));
    const auto payload = query.readSpan(query.readableBytes());
    if (query.failed())
        return std::unexpected(BackendLoginError::kMalformed);
    using Mode = wire::auth::forwarding::ForwardingMode;
    namespace basalt = wire::auth::forwarding::native;
    const bool velocityChannel =
        backend.forwarding == Mode::Velocity && std::ranges::equal(channel, velocity::kChannel);
    const bool nativeChannel = backend.forwarding == Mode::Basalt && std::ranges::equal(channel, basalt::kChannel);
    const bool forwarding = velocityChannel || nativeChannel;
    const auto requested = payload.empty() ? 1 : payload[0];
    // Velocity answers every request; Basalt's challenge is single use.
    if (nativeChannel && forwarded_)
        return std::unexpected(BackendLoginError::kMalformed);
    if (velocityChannel && (payload.size() > 1 || requested == 0))
        return std::unexpected(BackendLoginError::kMalformed);
    std::optional<basalt::Request> nativeRequest;
    if (nativeChannel) {
        nativeRequest = basalt::decodeRequest(payload);
        if (!nativeRequest || !native_)
            return std::unexpected(BackendLoginError::kMalformed);
    }
    frame_.clear();
    if (!frame_.tryReserve(forwarding ? kForwardingPacketLimit : kCapacity))
        return std::unexpected(BackendLoginError::kLimit);
    protocol::varInt::write(frame_, protocol::login::LoginPluginResponsePacket::kId);
    protocol::varInt::write(frame_, transaction);
    protocol::writeBoolean(frame_, forwarding);
    if (velocityChannel && !velocity::signInto(frame_, backend.secret, remoteAddress_, profile.id, profile.name,
                                               profile.properties, requested >= 4 ? 4 : 1))
        return std::unexpected(BackendLoginError::kMalformed);
    if (nativeChannel) {
        const bool javaEnvelope =
            native_->delegate && origin_ == 0 && backend.protocol == config::BackendConfig::kClientProtocol;
        const bool bedrockEnvelope = native_->delegateBedrock && origin_ == 1 && bedrockIdentity_.has_value();
        const bool envelope = (javaEnvelope || bedrockEnvelope) &&
                              (nativeRequest->capabilities & basalt::kCapabilitySessionEnvelope) != 0;
        envelopeSelected_ = envelope;
        basalt::Identity identity{.profileId = profile.id,
                                  .name = profile.name,
                                  .properties = profile.properties,
                                  .address = std::string{remoteAddress_},
                                  .port = remotePort_,
                                  .virtualHost = std::string{virtualHost_},
                                  .virtualPort = virtualPort_,
                                  .onlineMode = native_->onlineMode,
                                  .proxyId = native_->proxyId,
                                  .origin = static_cast<basalt::Origin>(origin_),
                                  .originId = originId_,
                                  .capabilities = envelope ? basalt::kCapabilitySessionEnvelope : 0U};
        const auto response = basalt::sign(native_->key, *nativeRequest, basalt::kVersion1, identity);
        if (response.empty())
            return std::unexpected(BackendLoginError::kMalformed);
        frame_.writeBytes(response);
    }
    if (auto result = queue(frame_.readable(), forwarding ? kForwardingPacketLimit : kCapacity); !result)
        return result;
    if (forwarding)
        forwarded_ = true;
    return {};
}

} // namespace basalt::proxy::runtime::detail
