#pragma once

#include <basalt/wire/wasm/Instance.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace basalt::proxy::plugins {

/// Interrupts the instance whose callback outlives its budget; asleep unless a callback runs.
class WasmWatchdog {
public:
    WasmWatchdog() : thread_([this](std::stop_token stop) { loop(stop); }) {}

    ~WasmWatchdog() {
        thread_.request_stop();
        wake_.notify_all();
    }

    WasmWatchdog(const WasmWatchdog&) = delete;
    WasmWatchdog& operator=(const WasmWatchdog&) = delete;

    void arm(wire::wasm::Instance& instance, std::chrono::milliseconds budget) {
        {
            const std::lock_guard lock{mutex_};
            target_ = &instance;
            deadline_ = std::chrono::steady_clock::now() + budget;
            ++generation_;
        }
        wake_.notify_one();
    }

    void disarm() {
        const std::lock_guard lock{mutex_};
        target_ = nullptr;
        ++generation_;
    }

private:
    void loop(std::stop_token stop) {
        std::unique_lock lock{mutex_};
        while (!stop.stop_requested()) {
            const std::uint64_t seen = generation_;
            if (target_ == nullptr) {
                wake_.wait(lock, stop, [&] { return generation_ != seen; });
                continue;
            }
            const auto deadline = deadline_;
            if (wake_.wait_until(lock, stop, deadline, [&] { return generation_ != seen; }) || stop.stop_requested())
                continue;
            target_->interrupt();
            target_ = nullptr;
        }
    }

    std::mutex mutex_;
    std::condition_variable_any wake_;
    wire::wasm::Instance* target_ = nullptr;
    std::chrono::steady_clock::time_point deadline_;
    std::uint64_t generation_ = 0;
    std::jthread thread_;
};

} // namespace basalt::proxy::plugins
