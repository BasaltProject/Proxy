#pragma once

#include "BackendAttempts.hpp"

#include <basalt/wire/common/Uuid.hpp>
#include <basalt/wire/io/ByteBuffer.hpp>

#include <optional>

namespace basalt::proxy::runtime::detail {

struct ResourcePackOrigin {
    wire::common::Uuid id;
    BackendAttempt backend;
};

/// Public pack IDs are session-local and never reused, including after returning to a backend.
class ResourcePacks {
public:
    explicit ResourcePacks(std::pmr::memory_resource& memory) : entries_(0, memory) {}

    [[nodiscard]] std::optional<wire::common::Uuid> publish(wire::common::Uuid id, BackendAttempt backend);
    [[nodiscard]] std::optional<ResourcePackOrigin> origin(wire::common::Uuid publicId) const;
    [[nodiscard]] std::optional<wire::common::Uuid> remove(wire::common::Uuid id, BackendAttempt backend);

    void clear() { entries_.clear(); }

    [[nodiscard]] std::size_t allocated() const { return entries_.capacity(); }

private:
    wire::io::ByteBuffer entries_;
    std::uint64_t sequence_ = 0;
};

} // namespace basalt::proxy::runtime::detail
