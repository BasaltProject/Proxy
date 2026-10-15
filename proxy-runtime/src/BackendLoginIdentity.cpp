#include "BackendLogin.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>

#include <algorithm>
#include <utility>

namespace basalt::proxy::runtime::detail {

std::expected<wire::protocol::login::LoginFinishedPacket, BackendLoginError>
BackendLogin::acceptLogin(wire::io::ByteBuffer& packet, const config::BackendConfig& backend,
                          const wire::auth::SessionProfile& profile) {
    using Mode = wire::auth::forwarding::ForwardingMode;
    namespace protocol = wire::protocol;
    if (phase_ != Phase::kLogin || hasCookieRequest() || packet.readableBytes() > kLoginPacketLimit ||
        (backend.forwarding == Mode::Velocity && !velocityForwarded_))
        return std::unexpected(BackendLoginError::kMalformed);
    const auto body = packet.readable();
    (void)protocol::readUuid(packet);
    const auto nameBytes = protocol::varInt::read(packet);
    if (packet.failed() || nameBytes < 0 || nameBytes > 16 * 3)
        return std::unexpected(BackendLoginError::kMalformed);
    (void)packet.readSpan(static_cast<std::size_t>(nameBytes));
    const auto count = protocol::varInt::read(packet);
    if (packet.failed() || count < 0 || count > 64)
        return std::unexpected(BackendLoginError::kMalformed);
    frame_.clear();
    if (!frame_.tryReserve(body.size()))
        return std::unexpected(BackendLoginError::kLimit);
    frame_.writeBytes(body);
    packet.skip(packet.readableBytes());
    auto finished = protocol::login::LoginFinishedPacket::read(frame_);
    if (!finished || frame_.readableBytes() != 0 || finished->gameProfile.name != profile.name ||
        (finished->gameProfile.id != profile.id &&
         (backend.forwarding != Mode::None ||
          finished->gameProfile.id != wire::common::Uuid::offlinePlayer(profile.name))))
        return std::unexpected(BackendLoginError::kMalformed);
    // Backend credentials must not become client profile properties.
    std::string_view tokenProperty;
    if (backend.forwarding == Mode::BungeeGuard)
        tokenProperty = wire::auth::forwarding::ProxyForwarding::kBungeeGuardProperty;
    else if (backend.forwarding == Mode::SafeNet)
        tokenProperty = backend.propertyName;
    if (!tokenProperty.empty())
        std::erase_if(finished->gameProfile.properties,
                      [tokenProperty](const auto& entry) { return entry.name == tokenProperty; });
    finished->gameProfile.id = profile.id;
    if (!advance(Phase::kLogin, Phase::kLoginAccepted))
        return std::unexpected(BackendLoginError::kMalformed);
    return std::move(*finished);
}

} // namespace basalt::proxy::runtime::detail
