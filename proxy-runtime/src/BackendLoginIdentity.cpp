#include "BackendLogin.hpp"
#include "LoginShapes.hpp"
#include "RouteVersions.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/multiversion/ProtocolVersions.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace basalt::proxy::runtime::detail {

std::expected<wire::protocol::login::LoginFinishedPacket, BackendLoginError>
BackendLogin::acceptLogin(wire::io::ByteBuffer& packet) {
    using Mode = wire::auth::forwarding::ForwardingMode;
    namespace protocol = wire::protocol;
    if (phase_ != Phase::kLogin || hasCookieRequest() || packet.readableBytes() > kLoginPacketLimit)
        return std::unexpected(BackendLoginError::kMalformed);
    const auto& backend = *backendConfig_;
    const auto& profile = *profile_;
    if ((backend.forwarding == Mode::Velocity || backend.forwarding == Mode::Basalt) && !forwarded_)
        return std::unexpected(BackendLoginError::kMalformed);
    std::span<const std::uint8_t> body = packet.readable();
    std::vector<std::uint8_t> converted;
    if (protocol_ < wire::multiversion::protocolVersions::kV1_20_2) {
        auto native = loginFinishedToNative(protocol_, body);
        if (!native)
            return std::unexpected(BackendLoginError::kMalformed);
        converted = std::move(*native);
        packet.skip(packet.readableBytes());
        packet.writeBytes(converted);
        body = packet.readable();
    }
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
    if (!frame_.tryReserve(body.size() + 16))
        return std::unexpected(BackendLoginError::kLimit);
    const auto tail = converted.empty() ? loginFinishedTail(protocol_) : LoginFinishedTail::kSessionId;
    frame_.writeBytes(tail == LoginFinishedTail::kStrictErrorsFlag && !body.empty() ? body.first(body.size() - 1)
                                                                                    : body);
    if (tail != LoginFinishedTail::kSessionId)
        for (int i = 0; i < 16; ++i)
            frame_.writeByte(0);
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
