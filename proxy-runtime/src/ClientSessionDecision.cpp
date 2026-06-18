#include "BackendLogin.hpp"
#include "BackendRoutes.hpp"
#include "ClientSession.hpp"

#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/play/ChatPackets.hpp>
#include <basalt/wire/protocol/play/CommandPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>
#include <basalt/wire/text/MiniMessage.hpp>

#include <algorithm>
#include <atomic>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::chrono::milliseconds kDecisionWindow{3000};
std::atomic<std::uint64_t> gNextToken{1};
} // namespace

bool ClientSession::ask(DecisionKind kind, std::string_view backend, std::string_view from, std::string_view text) {
    const bool anonymous = kind == DecisionKind::kPing;
    if (!decide_ || (!anonymous && !profile_) || decisionPending())
        return false;
    DecisionRequest request;
    request.token = gNextToken.fetch_add(1, std::memory_order_relaxed);
    request.kind = kind;
    if (profile_) {
        request.player = profile_->name;
        request.uuid = profile_->id.toString();
    }
    request.address = connection_.remoteAddress();
    request.port = connection_.remotePort();
    request.host = virtualHost_;
    request.backend = backend;
    request.from = from;
    request.text = text.substr(0, 1024);
    request.deadlineMs = static_cast<std::uint32_t>(kDecisionWindow.count());
    const auto token = request.token;
    if (!decide_(std::move(request)))
        return false;
    decisionToken_ = token;
    decisionKind_ = kind;
    decisionDeadline_ = Clock::now() + kDecisionWindow;
    if (state_ != State::kPlay && state_ != State::StatusRequest)
        decisionDeadline_ = std::min(decisionDeadline_, loginDeadline_);
    return true;
}

bool ClientSession::afterLogin() {
    if (ask(DecisionKind::kLogin, {}, {})) {
        deciding_ = true;
        return true;
    }
    return routeInitial();
}

bool ClientSession::routeInitial() {
    if (!preConnectAsked_ && !targets_.empty() && nextTarget_ < targets_.size()) {
        preConnectAsked_ = true;
        if (ask(DecisionKind::kPreConnect, backends_[targets_[nextTarget_]].name, {})) {
            deciding_ = true;
            return true;
        }
    }
    return beginJoin();
}

bool ClientSession::queueStatus(const DecisionResult* changes) {
    const std::vector<std::uint8_t> answer =
        status_(statusProtocol_, virtualHost_, connection_.remoteAddress(), changes);
    return !answer.empty() && queueOrdered(answer);
}

bool ClientSession::relayHeld() {
    if (!backend() || recovering_)
        return true;
    if (!toBackend(*backend(), held_))
        return false;
    backend()->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    backpressure();
    return true;
}

bool ClientSession::decided(std::uint64_t token, const DecisionResult& result, bool expired) {
    namespace protocol = wire::protocol;
    if (!awaiting(token) || closing_)
        return true;
    const bool inPlay = switchDecision_;
    const bool kicking = kickDeciding_;
    const auto kind = decisionKind_;
    deciding_ = false;
    switchDecision_ = false;
    kickDeciding_ = false;
    decisionDeadline_ = Clock::time_point::max();
    const auto redirect = [&]() -> std::optional<std::size_t> {
        return result.redirect.empty() ? std::nullopt : routes_.named(result.redirect);
    };
    if (kind == DecisionKind::kPing) {
        if (!queueStatus(expired ? nullptr : &result))
            return false;
        state_ = State::Ping;
        return true;
    }
    if (kicking) {
        const auto target = redirect();
        if (target && !expired) {
            held_.clear();
            redirected_[0] = *target;
            targets_ = redirected_;
            nextTarget_ = 0;
            return route();
        }
        recovering_ = false;
        closing_ = true;
        closeBackends();
        if (!expired && !result.text.empty()) {
            frame_.clear();
            protocol::varInt::write(frame_, protocol::play::PlayDisconnectPacket::kId);
            protocol::play::PlayDisconnectPacket{wire::text::miniMessage::parse(result.text)}.write(frame_);
            return queuePacket(frame_.readable());
        }
        return queuePacket(held_);
    }
    if (kind == DecisionKind::kChat || kind == DecisionKind::kCommand) {
        if (result.deny && !expired) {
            if (!result.reason.empty())
                tell(result.reason);
            return true;
        }
        if (kind == DecisionKind::kCommand && !expired && !result.text.empty()) {
            wire::io::ByteBuffer view;
            view.writeBytes(held_);
            if (protocol::varInt::read(view) == protocol::play::ChatCommandPacket::kId) {
                held_.clear();
                wire::io::ByteBuffer rebuilt;
                protocol::varInt::write(rebuilt, protocol::play::ChatCommandPacket::kId);
                protocol::writeString(rebuilt, result.text);
                held_.assign(rebuilt.readable().begin(), rebuilt.readable().end());
            }
        }
        return relayHeld();
    }
    if (result.deny && !expired) {
        if (inPlay) {
            tell(result.reason.empty() ? std::string_view{"<red>You cannot connect to that server."} : result.reason);
            return true;
        }
        closing_ = true;
        frame_.clear();
        protocol::varInt::write(frame_, protocol::login::LoginDisconnectPacket::kId);
        protocol::login::LoginDisconnectPacket{
            wire::text::miniMessage::parse(result.reason.empty() ? std::string_view{"Disconnected"} : result.reason)}
            .write(frame_);
        return queuePacket(frame_.readable());
    }
    const auto target = expired ? std::nullopt : redirect();
    if (inPlay) {
        if (expired)
            return true;
        if (!prepareBackend(target ? *target : decisionTarget_))
            return true;
        changeWhenPrepared_ = true;
        return true;
    }
    if (kind == DecisionKind::kLogin)
        return routeInitial();
    if (target) {
        redirected_[0] = *target;
        targets_ = redirected_;
        nextTarget_ = 0;
    }
    return beginJoin();
}

} // namespace basalt::proxy::runtime::detail
