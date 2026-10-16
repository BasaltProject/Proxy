#pragma once

#include <basalt/wire/auth/forwarding/BasaltForwarding.hpp>

#include <optional>
#include <string>

namespace basalt::proxy::runtime::detail {

struct NativeForwarding {
    wire::auth::forwarding::native::SigningKey key;
    std::string proxyId;
    bool onlineMode = false;
};

} // namespace basalt::proxy::runtime::detail
