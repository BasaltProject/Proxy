#include "BackendLogin.hpp"
#include "BackendRoutes.hpp"
#include "ClientSession.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>

namespace basalt::proxy::runtime::detail {

bool ClientSession::recoverBackend() {
    if (closing_ || recovering_ || state_ != State::kPlay || !activeBackend_ || startingConfiguration_)
        return false;
    discardCandidate();
    recoveryVisited_.set(activeBackend_->attempt().target);
    activeBackend_->retire();
    pendingKeepAlives_.clear();
    pendingPings_.clear();
    recovering_ = true;
    frontendPaused_ = false;
    targets_ = routes_.select(virtualHost_);
    nextTarget_ = 0;
    loginDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.loginTimeoutMs};
    readDeadline_ = loginDeadline_;
    changeWhenPrepared_ = true;
    return route();
}

bool ClientSession::recoveryFailed() {
    closing_ = true;
    closeBackends();
    frame_.clear();
    wire::protocol::varInt::write(frame_, wire::protocol::play::PlayDisconnectPacket::kId);
    wire::protocol::play::PlayDisconnectPacket{
        wire::text::Component::text("No backend is available. Please reconnect later.")}
        .write(frame_);
    return queuePacket(frame_.readable());
}

} // namespace basalt::proxy::runtime::detail
