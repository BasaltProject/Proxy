#pragma once

#include <basalt/wire/multiversion/ProtocolVersions.hpp>

#include <cstdint>
#include <string_view>

namespace basalt::proxy::runtime::buildInfo {

/// What this build is, for the banner and `--version`.
///
/// The version comes from the build (`BASALT_VERSION`, which also names the binary), so a release
/// and what it says about itself cannot drift apart.

#ifndef BASALT_VERSION
#define BASALT_VERSION "0.0.0-dev"
#endif

inline constexpr std::string_view kVersion = BASALT_VERSION;

inline constexpr std::string_view kMinecraftVersion = "26.3";

inline constexpr std::int32_t kProtocolVersion = wire::multiversion::protocolVersions::kNative;

} // namespace basalt::proxy::runtime::buildInfo
