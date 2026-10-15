#pragma once

#include <basalt/wire/io/ByteBuffer.hpp>
#include <basalt/wire/io/EventLoop.hpp>
#include <basalt/wire/io/ResourceBudget.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

namespace basalt::proxy::config {
struct RuntimeConfig;
}

namespace basalt::proxy::runtime::detail {

class ClientSession {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kBufferCapacity = 1024;
    static constexpr std::size_t kBufferBytes = kBufferCapacity * 3;
    ClientSession(wire::io::TcpConnection connection, wire::io::ResourceBudget::Reservation slot,
                  wire::io::ResourceBudget::Reservation buffers, const config::RuntimeConfig& settings,
                  std::span<const std::uint8_t> status, std::span<const std::uint8_t> unavailable,
                  Clock::time_point acceptedAt);

    [[nodiscard]] bool ready(const wire::io::IoReady& event);
    [[nodiscard]] wire::io::IoInterest interest() const;
    [[nodiscard]] Clock::time_point deadline() const;

    [[nodiscard]] std::int64_t handle() const { return connection_.nativeHandle(); }

    [[nodiscard]] bool accounted() const;
    std::uint64_t timer = 0;

private:
    enum class State { Handshake, StatusRequest, Ping };
    [[nodiscard]] bool decode();
    [[nodiscard]] bool packet();
    [[nodiscard]] bool queue(std::span<const std::uint8_t> bytes);
    wire::io::ResourceBudget::Reservation slot_;
    wire::io::ResourceBudget::Reservation buffers_;
    wire::io::TcpConnection connection_;
    const config::RuntimeConfig& settings_;
    std::span<const std::uint8_t> status_;
    std::span<const std::uint8_t> unavailable_;
    wire::io::ByteBuffer input_{kBufferCapacity};
    wire::io::ByteBuffer frame_{kBufferCapacity};
    wire::io::ByteBuffer output_{kBufferCapacity};
    State state_ = State::Handshake;
    bool closing_ = false;
    bool paused_ = false;
    Clock::time_point readDeadline_;
    Clock::time_point writeDeadline_ = Clock::time_point::max();
};

} // namespace basalt::proxy::runtime::detail
