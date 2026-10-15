#include "BackendChangeState.hpp"

#include <utility>

namespace basalt::proxy::runtime {

BackendChangeRequest::BackendChangeRequest(std::shared_ptr<detail::BackendChangeState> state)
    : state_(std::move(state)) {}

std::optional<BackendChangeResult> BackendChangeRequest::result() const {
    if (!state_)
        return BackendChangeResult::kStopped;
    if (state_->remaining.load(std::memory_order_acquire) == 0)
        return state_->outcome.load(std::memory_order_relaxed);
    if (state_->stopped->load(std::memory_order_acquire))
        return BackendChangeResult::kStopped;
    return std::nullopt;
}

} // namespace basalt::proxy::runtime
