#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>

namespace basalt::proxy::config {
struct AdmissionConfig;
}

namespace basalt::proxy::runtime {

enum class AdmissionResult { Accepted, GlobalRate, OriginRate, OriginCapacity, InvalidOrigin };

/// Shared admission gate for connection owners. Exhausted origin entries cannot be evicted
/// before refilling; rotating addresses cannot grow storage past the configured ceiling.
class Admission {
public:
    using Clock = std::chrono::steady_clock;
    explicit Admission(const config::AdmissionConfig& config, Clock::time_point start = Clock::now());
    ~Admission();
    Admission(const Admission&) = delete;
    Admission& operator=(const Admission&) = delete;

    [[nodiscard]] AdmissionResult attempt(std::string_view literalOrigin, Clock::time_point now = Clock::now());
    [[nodiscard]] std::size_t trackedOrigins() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace basalt::proxy::runtime
