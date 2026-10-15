#include "IdentityRegistry.hpp"

namespace basalt::proxy::runtime::detail {

IdentityRegistry::IdentityRegistry(std::size_t capacity) : capacity_(capacity) { identities_.reserve(capacity); }

bool IdentityRegistry::claim(const wire::common::Uuid& id) {
    std::lock_guard lock{mutex_};
    if (identities_.size() >= capacity_)
        return false;
    return identities_.insert(id).second;
}

void IdentityRegistry::release(const wire::common::Uuid& id) {
    std::lock_guard lock{mutex_};
    identities_.erase(id);
}

} // namespace basalt::proxy::runtime::detail
