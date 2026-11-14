#pragma once

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/auth/forwarding/BasaltForwarding.hpp>

#include <optional>
#include <string>

namespace basalt::proxy::runtime::detail {

struct NativeForwarding {
    wire::auth::forwarding::native::SigningKey key;
    std::string proxyId;
    bool onlineMode = false;
    bool delegate = true;
    bool delegateBedrock = false;
};

[[nodiscard]] constexpr bool delegatesTranslation(config::TranslationMode mode) noexcept {
    return mode == config::TranslationMode::Auto || mode == config::TranslationMode::Server;
}

} // namespace basalt::proxy::runtime::detail
