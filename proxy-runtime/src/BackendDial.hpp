#pragma once

#include <basalt/wire/io/TcpSocket.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <stop_token>
#include <vector>

namespace basalt::proxy::runtime::detail {

enum class DialState { kConnecting, kConnected };
enum class DialFailure { kInvalidEndpoints, kConnectFailed, kTimedOut, kCanceled, kInvalidState };

struct DialError {
    DialFailure kind;
    int platformCode = 0;
};

/// Owner-only TCP connection across resolved endpoints; DNS belongs to a bounded worker.
class BackendDial {
public:
    using Clock = std::chrono::steady_clock;
    BackendDial(std::vector<wire::io::TcpAddress> addresses, Clock::time_point deadline, std::stop_token stop = {});
    [[nodiscard]] std::expected<DialState, DialError> advance();
    [[nodiscard]] std::expected<wire::io::TcpConnection, DialError> take();

    [[nodiscard]] std::int64_t handle() const noexcept { return connection_.nativeHandle(); }

    [[nodiscard]] Clock::time_point deadline() const noexcept { return deadline_; }

private:
    std::vector<wire::io::TcpAddress> addresses_;
    Clock::time_point deadline_;
    std::stop_token stop_;
    wire::io::TcpConnection connection_;
    std::size_t next_ = 0;
    int lastError_ = 0;
    bool connected_ = false;
    bool taken_ = false;
    std::optional<DialError> failure_;
};

} // namespace basalt::proxy::runtime::detail
