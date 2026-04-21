#include "BackendLogin.hpp"
#include "ClientSession.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>

#include <array>

namespace basalt::proxy::runtime::detail {

ClientSession::CookiePublication ClientSession::publishCookieRequest(BackendLogin& source) {
    const auto keyBytes = source.pendingCookieKey();
    if (source.phase() != BackendLogin::Phase::kLogin || keyBytes.empty() || keyBytes.size() > 256 ||
        backendFor(source.attempt()) != &source)
        return CookiePublication::kFatal;
    const bool initial = state_ == State::kBackendLogin;
    if ((!initial && state_ != State::kConfiguration && state_ != State::kPlay) ||
        (initial && &source != candidateBackend_.get()))
        return CookiePublication::kFatal;
    std::array<char, 256> keyStorage{};
    for (std::size_t i = 0; i < keyBytes.size(); ++i)
        keyStorage[i] = static_cast<char>(keyBytes[i]);
    const std::string_view key{keyStorage.data(), keyBytes.size()};
    frame_.clear();
    if (!frame_.tryReserve(key.size() + 8) ||
        !pendingCookies_.add(key, cookieCache_.lookup(key).storeRevision, source.attempt()))
        return CookiePublication::kUnavailable;
    const auto id =
        initial ? wire::protocol::cookie::RequestPacket::kLoginId : (state_ == State::kConfiguration ? 0x00 : 0x15);
    wire::protocol::varInt::write(frame_, id);
    wire::protocol::writeString(frame_, key);
    if (!queuePacket(frame_.readable()))
        return CookiePublication::kFatal;
    if (initial)
        state_ = State::kCookieResponse;
    return CookiePublication::kPublished;
}

bool ClientSession::cookieResponse() {
    const bool initial = state_ == State::kCookieResponse;
    if (!initial && state_ != State::kConfiguration && state_ != State::kPlay)
        return false;
    auto response = wire::protocol::cookie::ResponsePacket::read(frame_);
    if (!response || frame_.readableBytes() != 0)
        return false;
    const auto request = pendingCookies_.consume(response->key);
    if (!request)
        return false;
    auto* destination = backendFor(request->backend);
    if (!destination)
        return !initial;
    if (initial && (destination != candidateBackend_.get() || destination->phase() != BackendLogin::Phase::kLogin))
        return false;
    if (destination->phase() == BackendLogin::Phase::kLogin) {
        if (!destination->answerCookie(*response, cookieCache_))
            return false;
    } else {
        if (destination->phase() != BackendLogin::Phase::kConfiguration &&
            destination->phase() != BackendLogin::Phase::kPlay)
            return false;
        if (cookieCache_.lookup(response->key).storeRevision == request->storeRevision &&
            !cookieCache_.remember(response->key, response->payload
                                                      ? std::optional{std::span<const std::uint8_t>{*response->payload}}
                                                      : std::nullopt))
            return false;
        if (!destination->relayCookie(*response))
            return false;
    }
    destination->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    backpressure();
    if (initial)
        state_ = State::kBackendLogin;
    else
        readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
    return true;
}

} // namespace basalt::proxy::runtime::detail
