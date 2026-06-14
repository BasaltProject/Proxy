#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::plugins {

inline constexpr std::string_view kBungeeChannel = "bungeecord:main";
inline constexpr std::string_view kLegacyBungeeChannel = "BungeeCord";

[[nodiscard]] bool isBungeeChannel(std::string_view channel) noexcept;

struct BungeePlayer {
    std::string name;
    std::string uuid;
    std::string server;
    std::string address;
    std::uint16_t port = 0;
};

/// What the BungeeCord plugin-messaging subchannels may read and do; every member is set by the host.
struct BungeeContext {
    std::function<std::optional<BungeePlayer>(std::string_view name)> player;
    /// Players on a backend, or on the whole network when the name is empty.
    std::function<std::vector<std::string>(std::string_view server)> playersOn;
    std::function<bool(std::string_view server)> serverExists;
    std::function<std::optional<std::pair<std::string, std::uint16_t>>(std::string_view server)> serverAddress;
    std::function<bool(std::string_view player, std::string_view server)> connect;
    std::function<void(std::string_view player, std::string_view reason)> kick;
    /// Plain text, never legacy codes: the host escapes it before it reaches the player.
    std::function<void(std::string_view player, std::string_view text)> tell;
    /// A component written as JSON, as the Raw subchannels carry it.
    std::function<void(std::string_view player, std::string_view json)> tellRaw;
    std::function<void(std::string_view player, std::string_view json)> kickRaw;
    /// A message toward the player's backend connection.
    std::function<void(std::string_view player, std::string_view channel, std::span<const std::uint8_t> data)>
        toBackend;
};

/// The BungeeCord channel, answered natively; the same operations a Lua plugin has through players, servers and
/// channels. Returns false when the payload is malformed or names no subchannel this proxy answers.
[[nodiscard]] bool handleBungeeMessage(const BungeeContext& context, std::string_view source,
                                       std::span<const std::uint8_t> payload);

/// The BungeeCord subchannels this proxy answers, for the adapter tables and the docs.
[[nodiscard]] std::span<const std::string_view> bungeeSubchannels() noexcept;

} // namespace basalt::proxy::plugins
