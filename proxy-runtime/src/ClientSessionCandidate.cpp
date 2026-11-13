#include "BackendLogin.hpp"
#include "BackendRoutes.hpp"
#include "ClientSession.hpp"
#include "RelayCounters.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/login/LoginPluginPackets.hpp>
#include <basalt/wire/text/ComponentJson.hpp>
#include <basalt/wire/text/Serializers.hpp>

#include <utility>

namespace basalt::proxy::runtime::detail {

namespace {

std::string disconnectReason(wire::io::ByteBuffer& frame) {
    const auto reason = wire::protocol::readString(frame);
    if (frame.failed())
        return {};
    const auto json = wire::common::parseJson(reason);
    return json ? wire::text::toPlainText(wire::text::componentJson::deserialize(*json)) : std::string{};
}

} // namespace

bool ClientSession::prepareBackend(std::size_t target) {
    if (closing_ || recovering_ || state_ != State::kPlay || Clock::now() >= deadline() || !activeBackend_ ||
        candidateBackend_ || startingConfiguration_ || connectionStage_ != ConnectionStage::kIdle ||
        target >= backends_.size() || target == activeBackend_->attempt().target)
        return false;
    requestedTarget_[0] = target;
    targets_ = requestedTarget_;
    nextTarget_ = 0;
    loginDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.loginTimeoutMs};
    if (!route())
        return false;
    return connectionStage_ != ConnectionStage::kIdle || candidateBackend_ != nullptr;
}

bool ClientSession::requestBackendChange(std::size_t target) {
    if (state_ == State::kPlay && !switchDecision_ && !deciding_ && target < backends_.size() &&
        ask(DecisionKind::kPreConnect, backends_[target].name, activeBackendName())) {
        decisionTarget_ = target;
        switchDecision_ = true;
        return true;
    }
    if (!prepareBackend(target))
        return false;
    changeWhenPrepared_ = true;
    return true;
}

bool ClientSession::requestBackendChange(std::string_view backend) {
    const auto target = routes_.named(backend);
    return target && requestBackendChange(*target);
}

bool ClientSession::advancePreparedChange() {
    if (!changeWhenPrepared_ || startingConfiguration_ || frontendPaused_ || !candidatePrepared())
        return true;
    return beginPreparedChange();
}

bool ClientSession::candidatePrepared() const {
    return !closing_ && state_ == State::kPlay && activeBackend_ && candidateBackend_ &&
           candidateBackend_->phase() == BackendLogin::Phase::kLoginAccepted && Clock::now() < deadline();
}

bool ClientSession::beginPreparedChange() {
    if (!candidatePrepared() || startingConfiguration_ || frontendPaused_)
        return false;
    if (legacyRaw_)
        return commitLegacyChange();
    if (legacyClient() && !clearClientState())
        return false;
    constexpr std::array<std::uint8_t, 1> start{0x78};
    if (!queuePacket(start))
        return false;
    startingConfiguration_ = true;
    uiLeavePlay();
    switchingBackend_ = true;
    constexpr std::int32_t kConfigurationAcknowledgedId = 0x10;
    return !legacyClient() || synthesizeClientPacket(kConfigurationAcknowledgedId);
}

bool ClientSession::commitLegacyChange() {
    if (!candidateBackend_->acknowledgeLogin() || !candidateBackend_->finishConfiguration() || !clearClientState())
        return false;
    resourcePacks_.clear();
    pendingKeepAlives_.clear();
    pendingPings_.clear();
    activeBackend_.reset();
    activeBackend_ = std::move(candidateBackend_);
    relayCounters().backendChanges.fetch_add(1, std::memory_order_relaxed);
    cancelConnectionAttempt();
    targets_ = {};
    nextTarget_ = 0;
    changeWhenPrepared_ = false;
    if (!recovering_)
        recoveryVisited_.reset();
    recovering_ = false;
    backendPaused_ = false;
    awaitingJoin_ = true;
    frontendPaused_ = true;
    readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
    activeBackend_->expectReadUntil(readDeadline_);
    activeBackend_->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    backpressure();
    return true;
}

bool ClientSession::acknowledgePreparedChange() {
    if (!switchingBackend_ || !startingConfiguration_ || !candidatePrepared() ||
        (output_.readableBytes() != 0 && !synthesizing_))
        return false;
    constexpr std::array<std::uint8_t, 1> ack{wire::protocol::login::kLoginAcknowledgedId};
    const bool bridged = bridges(candidateBackend_->protocol());
    if (bridged) {
        if (!candidateBackend_->acknowledgeLogin() || !candidateBackend_->finishConfiguration())
            return false;
    } else if (!toBackend(*candidateBackend_, ack) || !candidateBackend_->acknowledgeLogin() ||
               (clientSettings_.readableBytes() != 0 && !toBackend(*candidateBackend_, clientSettings_.readable()))) {
        return false;
    }
    state_ = State::kConfiguration;
    constexpr std::array<std::uint8_t, 2> removePacks{0x08, 0};
    constexpr std::array<std::uint8_t, 2> clearReportDetails{0x10, 0};
    constexpr std::array<std::uint8_t, 2> clearServerLinks{0x11, 0};
    constexpr std::array<std::uint8_t, 1> clearDialog{0x12};
    if (!queuePacket(removePacks) || !queuePacket(clearReportDetails) || !queuePacket(clearServerLinks) ||
        !queuePacket(clearDialog))
        return false;
    frame_.clear();
    wire::protocol::varInt::write(frame_, wire::protocol::configuration::BrandPacket::kId);
    wire::protocol::writeString(frame_, wire::protocol::configuration::BrandPacket::kChannel);
    wire::protocol::writeString(frame_, access_.settings && !access_.settings->general.brand.empty()
                                            ? access_.settings->general.brand
                                            : "Basalt Proxy");
    if (!queuePacket(frame_.readable()))
        return false;
    frame_.clear();
    wire::protocol::varInt::write(frame_, wire::protocol::configuration::UpdateEnabledFeaturesPacket::kId);
    wire::protocol::varInt::write(frame_, 1);
    wire::protocol::writeString(frame_, "minecraft:vanilla");
    if (!queuePacket(frame_.readable()))
        return false;
    // The public configuration ACK commits ownership of subsequent traffic.
    resourcePacks_.clear();
    pendingKeepAlives_.clear();
    pendingPings_.clear();
    activeBackend_.reset();
    activeBackend_ = std::move(candidateBackend_);
    relayCounters().backendChanges.fetch_add(1, std::memory_order_relaxed);
    cancelConnectionAttempt();
    targets_ = {};
    nextTarget_ = 0;
    startingConfiguration_ = false;
    switchingBackend_ = false;
    changeWhenPrepared_ = false;
    if (!recovering_)
        recoveryVisited_.reset();
    recovering_ = false;
    finishingConfiguration_ = false;
    bridgeWait_ = bridged;
    frontendPaused_ = false;
    backendPaused_ = false;
    readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
    activeBackend_->expectReadUntil(readDeadline_);
    activeBackend_->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    backpressure();
    return true;
}

void ClientSession::reportSwitchFailure(std::string_view reason) {
    if (!candidateBackend_ || !profile_)
        return;
    const std::string& server = backends_[candidateBackend_->attempt().target].name;
    const std::string shown = reason.empty() ? std::string{"the server did not accept the login"} : std::string{reason};
    wire::common::Logger{"Proxy"}.info(
        [&] { return profile_->name + " could not connect to " + server + ": " + shown; });
    const std::string text =
        kickText_ ? kickText_("connect-failed", {{"server", server}, {"reason", shown}}) : std::string{};
    if (text.empty())
        tell(wire::text::Component::text("Could not connect to " + server + ": " + shown));
    else
        tell(text);
}

void ClientSession::discardCandidate() {
    changeWhenPrepared_ = false;
    cancelConnectionAttempt();
    candidateBackend_.reset();
    targets_ = {};
    nextTarget_ = 0;
}

bool ClientSession::candidateReady(const wire::io::IoReady& event) {
    const auto reject = [this](std::string_view reason = {}) {
        if (switchingBackend_)
            return false;
        if (recovering_)
            return recoveryFailed();
        reportSwitchFailure(reason);
        discardCandidate();
        return true;
    };
    if (state_ != State::kPlay || !activeBackend_ || !candidateBackend_)
        return false;
    if (event.closed && candidateBackend_->phase() != BackendLogin::Phase::kLogin) {
        if (recovering_ && !switchingBackend_) {
            candidateBackend_.reset();
            return route();
        }
        return reject();
    }
    if (event.closed && !recovering_)
        return reject();
    auto& endpoint = *candidateBackend_;
    const auto packet = endpoint.ready(event, !backendPaused_ && endpoint.phase() == BackendLogin::Phase::kLogin &&
                                                  !endpoint.hasCookieRequest());
    if (!packet) {
        if (recovering_ && !switchingBackend_ && packet.error() == BackendLoginError::kIo) {
            candidateBackend_.reset();
            return route();
        }
        return reject();
    }
    if (packet->empty())
        return true;
    frame_.clear();
    if (!frame_.tryReserve(packet->size()))
        return reject();
    frame_.writeBytes(*packet);
    const auto id = wire::protocol::varInt::read(frame_);
    if (frame_.failed())
        return reject();
    if (id == wire::protocol::cookie::RequestPacket::kLoginId) {
        const auto action = endpoint.requestCookie(frame_, cookieCache_);
        if (!action)
            return reject();
        if (*action == BackendLogin::CookieAction::kRequestClient) {
            const auto publication = publishCookieRequest(endpoint);
            if (publication == CookiePublication::kUnavailable)
                return reject();
            return publication == CookiePublication::kPublished;
        }
    } else if (id == wire::protocol::login::LoginPluginRequestPacket::kId) {
        if (!endpoint.answerPluginQuery(frame_))
            return reject();
    } else if (id == wire::protocol::login::LoginFinishedPacket::kId) {
        if (!endpoint.acceptLogin(frame_))
            return reject();
        if (event.closed && recovering_ && !switchingBackend_) {
            candidateBackend_.reset();
            return route();
        }
    } else if (id == wire::protocol::login::LoginDisconnectPacket::kId) {
        return reject(disconnectReason(frame_));
    } else {
        return reject();
    }
    endpoint.expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    return true;
}

} // namespace basalt::proxy::runtime::detail
