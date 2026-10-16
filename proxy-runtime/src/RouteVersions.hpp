#pragma once

#include <cstdint>
#include <expected>

namespace basalt::proxy::runtime::detail {

enum class RouteRefusal { kClientUnsupported, kBackendUnsupported, kNoTranslation };

/// Client protocols with a route to a native backend that has been exercised against a real client or bot.
[[nodiscard]] bool translatedClientVerified(std::int32_t protocol) noexcept;

/// Older backend protocols a same-version client may reach untranslated, exercised against a real server and bot.
[[nodiscard]] bool backendVerified(std::int32_t protocol) noexcept;

/// From 26.2 the login success packet ends with the session id; 1.20.5 to 1.21.1 end with a strict-errors flag.
enum class LoginFinishedTail { kSessionId, kNone, kStrictErrorsFlag };
[[nodiscard]] LoginFinishedTail loginFinishedTail(std::int32_t protocol) noexcept;

/// Every pairing without a verified route is refused before any state is entered.
[[nodiscard]] std::expected<void, RouteRefusal> resolveRoute(std::int32_t clientProtocol,
                                                             std::int32_t backendProtocol) noexcept;

} // namespace basalt::proxy::runtime::detail
