#include "BackendAttempts.hpp"

#include <limits>

namespace basalt::proxy::runtime::detail {

std::optional<BackendAttempt> BackendAttempts::begin(std::size_t target) {
    active_.reset();
    if (generation_ == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    active_ = BackendAttempt{++generation_, target};
    return active_;
}

bool BackendAttempts::matches(BackendAttempt attempt) const { return active_ && *active_ == attempt; }

void BackendAttempts::clear() { active_.reset(); }

} // namespace basalt::proxy::runtime::detail
