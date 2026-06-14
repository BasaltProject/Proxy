#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::plugins {

enum class ForeignKind : std::uint8_t { kNone, kBungee, kVelocity };

struct DetectedJar {
    std::string path;
    ForeignKind kind = ForeignKind::kNone;
};

/// Decides by the descriptor inside the jar, never by its file name; a jar with neither descriptor is none.
[[nodiscard]] ForeignKind detectJar(std::string_view path);

/// The jars directly inside a directory that belong to a foreign proxy, sorted by path.
[[nodiscard]] std::vector<DetectedJar> detectJars(std::string_view directory);

} // namespace basalt::proxy::plugins
