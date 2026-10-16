#pragma once

#include <basalt/wire/auth/forwarding/BasaltForwarding.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace basalt::proxy::runtime::detail {

/// Reads the hex seed at path, or creates it with owner-only permissions when the file does not exist.
[[nodiscard]] std::expected<wire::auth::forwarding::native::SigningKey, std::string>
loadOrCreateForwardingKey(std::string_view path);

[[nodiscard]] std::string hexEncode(std::span<const std::uint8_t> bytes);

} // namespace basalt::proxy::runtime::detail
