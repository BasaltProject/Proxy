#pragma once

#include <basalt/wire/multiversion/ProtocolVersions.hpp>

#include <cstdint>
#include <string_view>

namespace basalt::proxy::runtime::buildInfo {

/// The version comes from the build (`BASALT_VERSION`), so a release and its banner cannot drift apart.
extern const std::string_view kVersion;

inline constexpr std::string_view kMinecraftVersion = "26.3";

inline constexpr std::int32_t kProtocolVersion = wire::multiversion::protocolVersions::kNative;

} // namespace basalt::proxy::runtime::buildInfo
