#include "BackendLogin.hpp"
#include "CookieCache.hpp"

#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>

#include <algorithm>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::size_t kCookiePacketLimit = wire::protocol::cookie::kMaxPayloadBytes + 512;
constexpr std::size_t kCookieKeyLimit = 256;
} // namespace

void BackendLogin::cancelCookieRequest() { cookieKey_.clear(); }

std::expected<void, BackendLoginError> BackendLogin::queueCookie(std::string_view key,
                                                                 std::optional<std::span<const std::uint8_t>> payload) {
    namespace protocol = wire::protocol;
    if ((phase_ != Phase::kLogin && phase_ != Phase::kConfiguration && phase_ != Phase::kPlay) || key.empty() ||
        key.size() > kCookieKeyLimit || (payload && payload->size() > protocol::cookie::kMaxPayloadBytes))
        return std::unexpected(BackendLoginError::kMalformed);
    auto id = phase_ == Phase::kLogin ? protocol::cookie::ResponsePacket::kLoginId
                                      : (phase_ == Phase::kConfiguration ? 0x01 : 0x15);
    if (ids_ && phase_ != Phase::kLogin) {
        id = ids_->toBackendId(phase_ == Phase::kConfiguration ? protocol::ProtocolState::Configuration
                                                               : protocol::ProtocolState::Play,
                               id);
        if (id < 0)
            return {};
    }
    frame_.clear();
    if (!frame_.tryReserve(kCookiePacketLimit))
        return std::unexpected(BackendLoginError::kLimit);
    protocol::varInt::write(frame_, id);
    protocol::writeString(frame_, key);
    protocol::writeBoolean(frame_, payload.has_value());
    if (payload)
        protocol::writeByteArray(frame_, *payload);
    return queue(frame_.readable(), kCookiePacketLimit);
}

std::expected<BackendLogin::CookieAction, BackendLoginError> BackendLogin::requestCookie(wire::io::ByteBuffer& packet,
                                                                                         const CookieCache& cache) {
    if (phase_ != Phase::kLogin || hasCookieRequest() || packet.readableBytes() > kCookiePacketLimit)
        return std::unexpected(BackendLoginError::kMalformed);
    auto request = wire::protocol::cookie::RequestPacket::read(packet);
    if (!request || packet.readableBytes() != 0 || request->key.size() > kCookieKeyLimit)
        return std::unexpected(BackendLoginError::kMalformed);
    const auto cached = cache.lookup(request->key);
    if (cached.known) {
        if (auto reply = queueCookie(request->key, cached.payload); !reply)
            return std::unexpected(reply.error());
        return CookieAction::kCached;
    }
    if (!cookieKey_.tryReserve(request->key.size()))
        return std::unexpected(BackendLoginError::kLimit);
    for (const auto byte : request->key)
        cookieKey_.writeByte(static_cast<std::uint8_t>(byte));
    cookieRevision_ = cached.storeRevision;
    return CookieAction::kRequestClient;
}

std::expected<void, BackendLoginError> BackendLogin::answerCookie(wire::io::ByteBuffer& packet, CookieCache& cache) {
    if (phase_ != Phase::kLogin || !hasCookieRequest() || packet.readableBytes() > kCookiePacketLimit)
        return std::unexpected(BackendLoginError::kMalformed);
    auto response = wire::protocol::cookie::ResponsePacket::read(packet);
    if (!response || packet.readableBytes() != 0)
        return std::unexpected(BackendLoginError::kMalformed);
    return answerCookie(*response, cache);
}

std::expected<void, BackendLoginError>
BackendLogin::answerCookie(const wire::protocol::cookie::ResponsePacket& response, CookieCache& cache) {
    if (phase_ != Phase::kLogin || !hasCookieRequest() || !std::ranges::equal(cookieKey_.readable(), response.key) ||
        (response.payload && response.payload->size() > wire::protocol::cookie::kMaxPayloadBytes))
        return std::unexpected(BackendLoginError::kMalformed);
    const auto payload =
        response.payload ? std::optional{std::span<const std::uint8_t>{*response.payload}} : std::nullopt;
    if (cache.lookup(response.key).storeRevision == cookieRevision_ && !cache.remember(response.key, payload))
        return std::unexpected(BackendLoginError::kLimit);
    if (auto reply = queueCookie(response.key, payload); !reply)
        return reply;
    cookieKey_.clear();
    return {};
}

std::expected<void, BackendLoginError>
BackendLogin::relayCookie(const wire::protocol::cookie::ResponsePacket& response) {
    if (phase_ != Phase::kConfiguration && phase_ != Phase::kPlay)
        return std::unexpected(BackendLoginError::kMalformed);
    return queueCookie(response.key, response.payload ? std::optional{std::span<const std::uint8_t>{*response.payload}}
                                                      : std::nullopt);
}

} // namespace basalt::proxy::runtime::detail
