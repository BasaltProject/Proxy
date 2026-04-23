#pragma once

#include <cstdint>
#include <expected>

namespace basalt::proxy::runtime::detail {

enum class RouteRefusal { kClientUnsupported, kBackendUnsupported, kNoTranslation };

/// Client protocols with a route to a native backend that has been exercised against a real client or bot.
[[nodiscard]] bool translatedClientVerified(std::int32_t protocol) noexcept;

/// Every pairing without a verified route is refused before any state is entered.
[[nodiscard]] std::expected<void, RouteRefusal> resolveRoute(std::int32_t clientProtocol,
                                                             std::int32_t backendProtocol) noexcept;

} // namespace basalt::proxy::runtime::detail
