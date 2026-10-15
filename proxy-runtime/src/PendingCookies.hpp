#pragma once

#include <basalt/wire/io/ByteBuffer.hpp>

#include <string_view>

namespace basalt::proxy::runtime::detail {

/// Owner-local correlation of cookie requests; storage uses the session's quota resource.
class PendingCookies {
public:
    explicit PendingCookies(std::pmr::memory_resource& memory) : keys_(0, memory) {}

    [[nodiscard]] bool add(std::string_view key);
    [[nodiscard]] bool consume(std::string_view key);

    [[nodiscard]] std::size_t allocated() const { return keys_.capacity(); }

private:
    wire::io::ByteBuffer keys_;
    std::size_t count_ = 0;
};

} // namespace basalt::proxy::runtime::detail
