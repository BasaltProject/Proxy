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

void ClientSession::cancelConnectionAttempt() {
    connectionStage_ = ConnectionStage::kIdle;
    attempts_.clear();
    dial_.reset();
    dialAttempt_.reset();
    resolutionStorage_.reset();
    resolutionLocal_.reset();
    if (resolution_)
        resolution_->request_stop();
    resolution_.reset();
}

bool ClientSession::route() {
    if (routeNext())
        return true;
    if (recovering_)
        return recoveryFailed();
    if (activeBackend_ && state_ == State::kPlay && !closing_) {
        discardCandidate();
        return true;
    }
    return false;
}

bool ClientSession::routeNext() {
    cancelConnectionAttempt();
    if (Clock::now() >= loginDeadline_)
        return false;
    if (!profile_)
        return false;
    if (!identityClaimed_) {
        if (!identities_.claim(profile_->id)) {
            closing_ = true;
            frame_.clear();
            wire::protocol::varInt::write(frame_, wire::protocol::login::LoginDisconnectPacket::kId);
            wire::protocol::login::LoginDisconnectPacket{
                wire::text::Component::text("This player is already connected.")}
                .write(frame_);
            return queuePacket(frame_.readable());
        }
        identityClaimed_ = true;
    }
    while (nextTarget_ < targets_.size() && ((recovering_ && recoveryVisited_.test(targets_[nextTarget_])) ||
                                             !acceptsBackend(backends_[targets_[nextTarget_]])))
        ++nextTarget_;
    if (nextTarget_ == targets_.size()) {
        if (recovering_)
            return recoveryFailed();
        if (activeBackend_ && state_ == State::kPlay) {
            discardCandidate();
            return true;
        }
        closing_ = true;
        return queue(unavailable_);
    }
    if (settings_.sessionBufferBytes <
        kBufferBytes + kResolutionBytes + (profileStorage_ ? profileStorage_->size() : 0))
        return false;
    connectDeadline_ = std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.connectTimeoutMs});
    resolutionLocal_ = localBuffers_.acquire(kResolutionBytes);
    if (!resolutionLocal_)
        return false;
    const auto target = targets_[nextTarget_++];
    if (recovering_)
        recoveryVisited_.set(target);
    auto attempt = attempts_.begin(target);
    if (!attempt)
        return false;
    resolution_ = resolve_(*attempt, connectDeadline_);
    if (!resolution_)
        return false;
    connectionStage_ = ConnectionStage::kResolving;
    if (!activeBackend_)
        state_ = State::kBackendLogin;
    return true;
}

bool ClientSession::resolved(BackendAttempt attempt, std::vector<wire::io::TcpAddress> addresses,
                             wire::io::ResourceBudget::Reservation storage) {
    // A canceled lookup can finish after the same backend has been selected again.
    if (closing_ || connectionStage_ != ConnectionStage::kResolving || !attempts_.matches(attempt))
        return true;
    if (Clock::now() >= deadline())
        return timedOut(Clock::now());
    attempts_.clear();
    resolution_.reset();
    if (addresses.empty()) {
        storage.reset();
        return route();
    }
    if (addresses.size() > 32 || addresses.capacity() > storage.size() / sizeof(wire::io::TcpAddress))
        return false;
    if (!storage.resize(addresses.capacity() * sizeof(wire::io::TcpAddress)) || !resolutionLocal_ ||
        !resolutionLocal_->resize(addresses.capacity() * sizeof(wire::io::TcpAddress)))
        return false;
    resolutionStorage_ = std::move(storage);
    dialAttempt_ = attempt;
    dial_ = std::make_unique<BackendDial>(std::move(addresses), connectDeadline_);
    connectionStage_ = ConnectionStage::kConnecting;
    return dial();
}

bool ClientSession::dial() {
    auto state = dial_->advance();
    if (!state)
        return route();
    if (*state == DialState::kConnecting)
        return true;
    auto connected = dial_->take();
    if (!connected)
        return route();
    dial_.reset();
    connectionStage_ = ConnectionStage::kIdle;
    resolutionStorage_.reset();
    resolutionLocal_.reset();
    if (!dialAttempt_ || dialAttempt_->target >= backends_.size())
        return false;
    candidateBackend_ = std::make_unique<BackendLogin>(std::move(*connected), memory_, *dialAttempt_);
    const std::int32_t speaks = speaksTo(backends_[candidateBackend_->attempt().target], clientProtocol_);
    candidateBackend_->useIds(rawProtocol(speaks) ? ids_.get() : nullptr);
    dialAttempt_.reset();
    if (!profile_ ||
        !candidateBackend_->start(backends_[candidateBackend_->attempt().target], *profile_, virtualHost_, virtualPort_,
                                  connection_.remoteAddress(), connection_.remotePort(), nativeForwarding_, speaks)) {
        if (recovering_)
            return recoveryFailed();
        if (activeBackend_ && state_ == State::kPlay) {
            discardCandidate();
            return true;
        }
        closeBackends();
        closing_ = true;
        return queue(unavailable_);
    }
    if (!activeBackend_)
        state_ = State::kBackendLogin;
    candidateBackend_->boundPreparation(loginDeadline_);
    candidateBackend_->expectWriteUntil(
        std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs}));
    return true;
}

BackendLogin* ClientSession::backend() const {
    // Frontend traffic remains attached to the active backend during candidate preparation.
    return activeBackend_ ? activeBackend_.get() : candidateBackend_.get();
}

BackendLogin* ClientSession::backendFor(BackendAttempt attempt) const {
    for (auto* endpoint : {activeBackend_.get(), candidateBackend_.get()})
        if (endpoint && !(recovering_ && endpoint == activeBackend_.get()) && endpoint->attempt() == attempt)
            return endpoint;
    return nullptr;
}

ClientSession::Clock::time_point ClientSession::backendDeadline() const {
    auto result = Clock::time_point::max();
    const std::array endpoints{activeBackend_.get(), candidateBackend_.get()};
    for (const auto* endpoint : endpoints)
        if (endpoint && !(recovering_ && endpoint == activeBackend_.get()))
            result = std::min(result, endpoint->deadline(endpoint == backend() && backendPaused_));
    return result;
}

void ClientSession::closeBackends() {
    cancelConnectionAttempt();
    candidateBackend_.reset();
    activeBackend_.reset();
}

std::array<ClientSession::BackendWatch, 3> ClientSession::backendWatches() const {
    std::array<BackendWatch, 3> result{};
    if (dial_ && dial_->handle() >= 0)
        result[0] = {dial_->handle(), {false, true}};
    const std::array endpoints{activeBackend_.get(), candidateBackend_.get()};
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        auto* endpoint = endpoints[i];
        if (!endpoint)
            continue;
        const bool selected = endpoint == backend();
        const bool preparing = activeBackend_ && endpoint == candidateBackend_.get() && state_ == State::kPlay;
        const bool read = (selected || (preparing && endpoint->phase() == BackendLogin::Phase::kLogin)) &&
                          !backendPaused_ && !endpoint->hasCookieRequest() && state_ != State::kLoginAck &&
                          state_ != State::kCookieResponse && !startingConfiguration_;
        const auto interest = endpoint->interest(read);
        if ((selected && !backendPaused_) || preparing || interest.readable || interest.writable)
            result[i + 1] = {endpoint->handle(), interest};
    }
    return result;
}

void ClientSession::backpressure() {
    if (awaitingJoin_ || (backend() && backend()->pendingBytes() >= settings_.highWaterBytes))
        frontendPaused_ = true;
    else if (frontendPaused_ && (!backend() || backend()->pendingBytes() <= settings_.lowWaterBytes)) {
        frontendPaused_ = false;
        if (state_ == State::kConfiguration || state_ == State::kPlay)
            readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
    }
    if (output_.readableBytes() >= settings_.highWaterBytes)
        backendPaused_ = true;
    else if (backendPaused_ && output_.readableBytes() <= settings_.lowWaterBytes) {
        backendPaused_ = false;
        if (state_ == State::kConfiguration || state_ == State::kPlay)
            if (backend())
                backend()->expectReadUntil(Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs});
    }
}

bool ClientSession::bufferedPacket() const {
    if (state_ != State::kConfiguration && state_ != State::kPlay)
        return false;
    auto hint = wire::protocol::packetFraming::inspectFrame(
        input_.readable(), compression_.has_value(), wire::protocol::packetFraming::kMaxUncompressedSize, compression_);
    return !hint || (*hint && (**hint).complete);
}

} // namespace basalt::proxy::runtime::detail
