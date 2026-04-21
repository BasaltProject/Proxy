#include "BackendLogin.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/auth/forwarding/VelocityForwarding.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/login/LoginPluginPackets.hpp>

#include <algorithm>

namespace basalt::proxy::runtime::detail {

std::expected<void, BackendLoginError> BackendLogin::answerPluginQuery(wire::io::ByteBuffer& query,
                                                                       const config::BackendConfig& backend,
                                                                       const wire::auth::SessionProfile& profile,
                                                                       std::string_view remoteAddress) {
    namespace protocol = wire::protocol;
    namespace velocity = wire::auth::forwarding::velocity;
    if (phase_ != Phase::kLogin || hasCookieRequest() || query.readableBytes() > kLoginPacketLimit)
        return std::unexpected(BackendLoginError::kMalformed);
    const auto transaction = protocol::varInt::read(query);
    const auto channelBytes = protocol::varInt::read(query);
    if (query.failed() || channelBytes < 0 || channelBytes > 32767 * 3)
        return std::unexpected(BackendLoginError::kMalformed);
    const auto channel = query.readSpan(static_cast<std::size_t>(channelBytes));
    const auto payload = query.readSpan(query.readableBytes());
    if (query.failed())
        return std::unexpected(BackendLoginError::kMalformed);
    const bool forwarding = backend.forwarding == wire::auth::forwarding::ForwardingMode::Velocity &&
                            std::ranges::equal(channel, velocity::kChannel);
    const auto requested = payload.empty() ? 1 : payload[0];
    if (forwarding && (velocityForwarded_ || payload.size() > 1 || requested == 0))
        return std::unexpected(BackendLoginError::kMalformed);
    frame_.clear();
    if (!frame_.tryReserve(forwarding ? kForwardingPacketLimit : kCapacity))
        return std::unexpected(BackendLoginError::kLimit);
    protocol::varInt::write(frame_, protocol::login::LoginPluginResponsePacket::kId);
    protocol::varInt::write(frame_, transaction);
    protocol::writeBoolean(frame_, forwarding);
    if (forwarding && !velocity::signInto(frame_, backend.secret, remoteAddress, profile.id, profile.name,
                                          profile.properties, requested >= 4 ? 4 : 1))
        return std::unexpected(BackendLoginError::kMalformed);
    if (auto result = queue(frame_.readable(), forwarding ? kForwardingPacketLimit : kCapacity); !result)
        return result;
    if (forwarding)
        velocityForwarded_ = true;
    return {};
}

} // namespace basalt::proxy::runtime::detail
