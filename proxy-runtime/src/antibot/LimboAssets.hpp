#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace basalt::proxy::runtime::antibot {

inline constexpr double kLimboFloorY = 64.0;
inline constexpr double kLimboSpawnX = 8.5;
inline constexpr double kLimboSpawnZ = 8.5;
inline constexpr std::int32_t kLimboTeleportId = 1;

/// The fixed packets of the limbo for one protocol version, already translated and framed for the wire.
struct LimboPackets {
    std::vector<std::uint8_t> configIntro;
    std::vector<std::uint8_t> configRest;
    std::vector<std::uint8_t> playIntro;
    std::vector<std::uint8_t> startConfiguration;
};

/// Builds each protocol's packets once and shares them with every client that speaks it.
class LimboAssets {
public:
    explicit LimboAssets(std::int32_t compressionThreshold) : threshold_(compressionThreshold) {}

    /// Null for a protocol the limbo cannot serve.
    [[nodiscard]] std::shared_ptr<const LimboPackets> forProtocol(std::int32_t protocol, std::int32_t dropHeight);

    [[nodiscard]] static bool serves(std::int32_t protocol);

private:
    std::int32_t threshold_;
    std::mutex mutex_;
    std::map<std::pair<std::int32_t, std::int32_t>, std::shared_ptr<const LimboPackets>> cache_;
};

} // namespace basalt::proxy::runtime::antibot
