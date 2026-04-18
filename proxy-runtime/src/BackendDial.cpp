#include "BackendDial.hpp"

#include <algorithm>

namespace basalt::proxy::runtime::detail {

BackendDial::BackendDial(std::vector<wire::io::TcpAddress> addresses, Clock::time_point deadline, std::stop_token stop)
    : addresses_(std::move(addresses)), deadline_(deadline), stop_(stop) {
    if (addresses_.empty() || addresses_.size() > 32 ||
        std::ranges::any_of(addresses_, [](const auto& address) { return address.port() == 0; }))
        failure_ = DialError{DialFailure::kInvalidEndpoints};
}

std::expected<DialState, DialError> BackendDial::advance() {
    const auto fail = [&](DialError error) -> std::expected<DialState, DialError> {
        connection_.close();
        failure_ = error;
        return std::unexpected(error);
    };
    if (taken_)
        return std::unexpected(DialError{DialFailure::kInvalidState});
    if (stop_.stop_requested())
        return fail({DialFailure::kCanceled});
    if (Clock::now() >= deadline_)
        return fail({DialFailure::kTimedOut});
    if (failure_)
        return std::unexpected(*failure_);
    if (connected_)
        return DialState::kConnected;
    while (true) {
        if (stop_.stop_requested())
            return fail({DialFailure::kCanceled});
        if (Clock::now() >= deadline_)
            return fail({DialFailure::kTimedOut});
        if (!connection_.isClosed()) {
            const auto finished = connection_.finishConnect();
            if (finished && *finished) {
                connected_ = true;
                return DialState::kConnected;
            }
            if (finished)
                return DialState::kConnecting;
            lastError_ = finished.error().platformCode;
            connection_.close();
        }
        if (next_ == addresses_.size())
            return fail({DialFailure::kConnectFailed, lastError_});
        auto started = wire::io::TcpConnection::connectNonBlocking(addresses_[next_++]);
        if (!started) {
            lastError_ = started.error().platformCode;
            continue;
        }
        connection_ = std::move(*started);
    }
}

std::expected<wire::io::TcpConnection, DialError> BackendDial::take() {
    if (!connected_ || taken_ || failure_)
        return std::unexpected(DialError{DialFailure::kInvalidState});
    if (stop_.stop_requested() || Clock::now() >= deadline_) {
        connection_.close();
        failure_ = DialError{stop_.stop_requested() ? DialFailure::kCanceled : DialFailure::kTimedOut};
        return std::unexpected(*failure_);
    }
    taken_ = true;
    return std::move(connection_);
}

} // namespace basalt::proxy::runtime::detail
