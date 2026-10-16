#pragma once

#include "BackendAttempts.hpp"

#include <basalt/wire/io/ByteBuffer.hpp>

#include <limits>
#include <optional>

namespace basalt::proxy::runtime::detail {

struct ResponseOrigin {
    std::int64_t token;
    BackendAttempt backend;
};

class PendingResponses {
public:
    explicit PendingResponses(std::pmr::memory_resource& memory,
                              std::uint64_t limit = std::numeric_limits<std::int64_t>::max())
        : entries_(0, memory), limit_(limit) {}

    [[nodiscard]] std::optional<std::int64_t> publish(std::int64_t token, BackendAttempt backend);
    [[nodiscard]] std::optional<ResponseOrigin> consume(std::int64_t publicToken);

    void clear() { entries_.clear(); }

    [[nodiscard]] std::size_t allocated() const { return entries_.capacity(); }

private:
    wire::io::ByteBuffer entries_;
    std::uint64_t sequence_ = 0;
    std::uint64_t limit_;
};

} // namespace basalt::proxy::runtime::detail
