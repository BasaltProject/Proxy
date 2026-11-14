#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::plugins {

enum class ForeignKind : std::uint8_t { kNone, kBungee, kVelocity };

/// Lowercase plugin id, descriptor name or jar file name to the facade a jar with both descriptors goes to.
using PlatformChoices = std::map<std::string, ForeignKind, std::less<>>;

struct DetectedJar {
    std::string path;
    ForeignKind kind = ForeignKind::kNone;
};

/// Decides by the descriptors inside the jar, never by its file name alone; a jar with neither descriptor is none.
[[nodiscard]] ForeignKind detectJar(std::string_view path, const PlatformChoices& choices = {});

/// The jars directly inside a directory that belong to a foreign proxy, sorted by path.
[[nodiscard]] std::vector<DetectedJar> detectJars(std::string_view directory, const PlatformChoices& choices = {});

} // namespace basalt::proxy::plugins
