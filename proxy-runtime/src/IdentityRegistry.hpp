#pragma once

#include <basalt/wire/common/Uuid.hpp>

#include <mutex>
#include <unordered_set>

namespace basalt::proxy::runtime::detail {

/// First verified session owns its UUID until destruction. Shared only during login and close.
class IdentityRegistry {
public:
    explicit IdentityRegistry(std::size_t capacity);
    [[nodiscard]] bool claim(const wire::common::Uuid& id);
    void release(const wire::common::Uuid& id);

private:
    std::mutex mutex_;
    std::unordered_set<wire::common::Uuid> identities_;
    std::size_t capacity_;
};

} // namespace basalt::proxy::runtime::detail
