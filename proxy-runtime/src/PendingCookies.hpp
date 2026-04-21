#pragma once

#include "BackendAttempts.hpp"

#include <basalt/wire/io/ByteBuffer.hpp>

#include <optional>
#include <string_view>

namespace basalt::proxy::runtime::detail {

struct PendingCookie {
    std::uint64_t storeRevision;
    BackendAttempt backend;
    friend bool operator==(const PendingCookie&, const PendingCookie&) = default;
};

/// Owner-local correlation of cookie requests; storage uses the session's quota resource.
class PendingCookies {
public:
    explicit PendingCookies(std::pmr::memory_resource& memory) : keys_(0, memory) {}

    [[nodiscard]] bool add(std::string_view key, std::uint64_t storeRevision, BackendAttempt backend);
    [[nodiscard]] std::optional<PendingCookie> consume(std::string_view key);

    [[nodiscard]] std::size_t allocated() const { return keys_.capacity(); }

private:
    wire::io::ByteBuffer keys_;
    std::size_t count_ = 0;
};

} // namespace basalt::proxy::runtime::detail
