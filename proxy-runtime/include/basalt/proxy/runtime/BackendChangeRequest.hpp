#pragma once

#include <memory>
#include <optional>

namespace basalt::proxy::runtime {
namespace detail {
struct BackendChangeState;
}

class ProxyRuntime;

enum class BackendChangeError { kNotRunning, kInvalidPlayer, kUnknownBackend, kBusy };
enum class BackendChangeResult { kAccepted, kPlayerNotFound, kUnavailable, kStopped };

/// Accepted means preparation started, not that the player has entered the destination.
class BackendChangeRequest {
public:
    [[nodiscard]] std::optional<BackendChangeResult> result() const;

private:
    explicit BackendChangeRequest(std::shared_ptr<detail::BackendChangeState> state);
    std::shared_ptr<detail::BackendChangeState> state_;
    friend class ProxyRuntime;
};

} // namespace basalt::proxy::runtime
