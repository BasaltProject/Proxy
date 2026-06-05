#pragma once

#include <string_view>

namespace basalt::proxy::runtime::detail::kicks {

inline constexpr std::string_view kConnectionThrottled = "Connection throttled! Please wait before reconnecting.";
inline constexpr std::string_view kInvalidName = "Invalid characters in username";
inline constexpr std::string_view kPacketRate = "Kicked for exceeding packet rate limit";
inline constexpr std::string_view kSpam = "Kicked for spamming";
inline constexpr std::string_view kUnsignedChat = "Received a chat packet with a missing or invalid signature.";

} // namespace basalt::proxy::runtime::detail::kicks
