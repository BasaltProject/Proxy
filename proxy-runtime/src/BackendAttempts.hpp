#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace basalt::proxy::runtime::detail {

struct BackendAttempt {
    std::uint64_t generation;
    std::size_t target;
    friend bool operator==(const BackendAttempt&, const BackendAttempt&) = default;
};

/// Owner-local correlation. Revisiting a backend never reuses the previous attempt's authority.
class BackendAttempts {
public:
    [[nodiscard]] std::optional<BackendAttempt> begin(std::size_t target);
    [[nodiscard]] bool matches(BackendAttempt attempt) const;
    void clear();

private:
    std::uint64_t generation_ = 0;
    std::optional<BackendAttempt> active_;
};

} // namespace basalt::proxy::runtime::detail
