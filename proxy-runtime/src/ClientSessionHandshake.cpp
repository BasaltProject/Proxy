#include "BackendRoutes.hpp"
#include "ClientSession.hpp"
#include "LoginShapes.hpp"
#include "RouteVersions.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/auth/SessionKeyPair.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/handshake/HandshakePacket.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>

#include <algorithm>
#include <array>

namespace basalt::proxy::runtime::detail {

/// Everything a client sends before it has a backend: the handshake, the login start, the encryption response and the
/// status exchange.
bool ClientSession::preLoginPacket(std::int32_t id) {
    namespace protocol = wire::protocol;
    if (state_ == State::Handshake) {
        if (id != protocol::handshake::HandshakePacket::kId)
            return false;
        const auto hello = protocol::handshake::HandshakePacket::read(frame_);
        if (!hello || frame_.readableBytes() != 0 || hello->hostName.empty() ||
            hello->hostName.find('\0') != std::string::npos)
            return false;
        if (hello->intent == protocol::handshake::ClientIntent::Status) {
            virtualHost_ = hello->hostName;
            statusProtocol_ = hello->protocolVersion;
            state_ = State::StatusRequest;
        } else if ((hello->intent == protocol::handshake::ClientIntent::Login ||
                    (acceptTransfers_ && hello->intent == protocol::handshake::ClientIntent::Transfer)) &&
                   (hello->protocolVersion == buildInfo::kProtocolVersion ||
                    translatedClientVerified(hello->protocolVersion) || backendVerified(hello->protocolVersion))) {
            const auto candidates = routes_.select(hello->hostName);
            if (!candidates.empty() && std::ranges::none_of(candidates, [&](std::size_t target) {
                    return resolveRoute(hello->protocolVersion,
                                        effectiveProtocol(backends_[target].protocol, hello->protocolVersion))
                        .has_value();
                }))
                return false;
            if (hello->protocolVersion != buildInfo::kProtocolVersion) {
                clientProtocol_ = hello->protocolVersion;
                auto chain = wire::multiversion::java::javaTranslators::chainFor(hello->protocolVersion);
                if (!chain)
                    return false;
                const auto reaches = [&](std::int32_t protocol) {
                    return std::ranges::any_of(candidates, [&](std::size_t target) {
                        return speaksTo(backends_[target].protocol, hello->protocolVersion) == protocol;
                    });
                };
                const bool rawRoute = backendVerified(hello->protocolVersion) && reaches(hello->protocolVersion);
                const bool translatedRoute = reaches(buildInfo::kProtocolVersion);
                if (rawRoute) {
                    ids_ = std::make_unique<IdRenumbering>(std::move(chain));
                    if (hello->protocolVersion < wire::multiversion::protocolVersions::kV1_20_2) {
                        translation_ = wire::multiversion::java::javaTranslators::chainFor(hello->protocolVersion);
                        legacyRaw_ = translation_ != nullptr;
                        if (!legacyRaw_)
                            return false;
                    } else if (translatedRoute && translatedClientVerified(hello->protocolVersion)) {
                        translation_ = wire::multiversion::java::javaTranslators::chainFor(hello->protocolVersion);
                    }
                } else {
                    translation_ = std::move(chain);
                }
            }
            state_ = State::LoginStart;
            virtualHost_ = hello->hostName;
            virtualPort_ = hello->port;
            targets_ = candidates;
            loginDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.loginTimeoutMs};
            readDeadline_ = loginDeadline_;
            return true;
        } else {
            return false;
        }
    } else if (state_ == State::LoginStart) {
        if (id != protocol::login::LoginStartPacket::kId)
            return false;
        auto loginName = readClientLoginName(
            translation_ || clientProtocol_ == 0 ? buildInfo::kProtocolVersion : clientProtocol_, frame_);
        if (!loginName || !wire::auth::validLoginName(*loginName))
            return false;
        name_ = std::move(*loginName);
        if (!online_) {
            profile_ = wire::auth::SessionProfile{wire::common::Uuid::offlinePlayer(name_), name_, {}};
            if (playerLogin_)
                playerLogin_(profile_->name, profile_->id.toString());
            return route();
        }
        if (settings_.sessionBufferBytes < kBufferBytes + kAuthenticationBytes || publicKey_.empty() ||
            !wire::auth::randomBytes(challenge_))
            return false;
        protocol::login::HelloPacket hello{.serverId = "",
                                           .publicKey = {publicKey_.begin(), publicKey_.end()},
                                           .challenge = {challenge_.begin(), challenge_.end()},
                                           .shouldAuthenticate = true};
        frame_.clear();
        protocol::varInt::write(frame_, protocol::login::HelloPacket::kId);
        writeEncryptionRequest(frame_, clientProtocol_ != 0 ? clientProtocol_ : buildInfo::kProtocolVersion, hello);
        output_.discardReadBytes();
        if (frame_.readableBytes() + 3 > kBufferCapacity - output_.writerIndex())
            return false;
        if (output_.readableBytes() == 0)
            writeDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs};
        if (!protocol::packetFraming::writeUncompressed(output_, frame_.readable()))
            return false;
        state_ = State::Key;
        return true;
    } else if (state_ == State::Key) {
        if (id != protocol::login::KeyPacket::kId)
            return false;
        auto key = readEncryptionResponse(
            translation_ || clientProtocol_ == 0 ? buildInfo::kProtocolVersion : clientProtocol_, frame_);
        if (!key || frame_.readableBytes() != 0 || key->encryptedSecret.size() != 128 ||
            key->encryptedChallenge.size() != 128 || output_.readableBytes() != 0)
            return false;
        wire::auth::OnlineLoginRequest request{name_,
                                               checkAddress_ ? connection_.remoteAddress() : std::string{},
                                               challenge_,
                                               std::move(key->encryptedSecret),
                                               std::move(key->encryptedChallenge),
                                               loginDeadline_};
        profileLocal_ = localBuffers_.acquire(kAuthenticationBytes);
        if (!profileLocal_)
            return false;
        authentication_ = authenticate_(std::move(request));
        if (!authentication_)
            return false;
        state_ = State::Authenticating;
        return true;
    } else if (state_ == State::StatusRequest) {
        if (id != protocol::status::kStatusRequestId || frame_.readableBytes() != 0 || !queue(status_))
            return false;
        if (statusPing_)
            statusPing_(virtualHost_, statusProtocol_);
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

} // namespace basalt::proxy::runtime::detail
