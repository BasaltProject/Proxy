#pragma once

#include <basalt/proxy/plugins/PluginHost.hpp>
#include <basalt/wire/common/toml/Toml.hpp>
#include <basalt/wire/plugins/CommandSpec.hpp>
#include <basalt/wire/text/Component.hpp>

namespace basalt::proxy::plugins {

inline constexpr std::chrono::milliseconds kTick{50};

/// The dispatcher node for a plugin command: its literals, then typed arguments, running `executor` at the end.
[[nodiscard]] runtime::CommandNode commandNode(const wire::plugins::CommandSpec& spec,
                                               runtime::CommandExecutor executor);

/// A component read from JSON text, or nothing when the text is not valid component JSON.
[[nodiscard]] std::optional<wire::text::Component> componentOfJson(std::string_view json);

/// The PNG bytes of a `data:image/png;base64,` icon; empty for an empty text, nothing when it does not decode.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> faviconOf(std::string_view dataUri);

/// Limits on what a ping decision may put in the server list.
inline constexpr std::size_t kMaxSampleLines = 32;
inline constexpr std::size_t kMaxSampleLine = 256;

/// The bit a decision event sets in PluginHost::deciders_, or 0 when the event is not a decision.
[[nodiscard]] unsigned decisionBit(std::string_view event);
[[nodiscard]] unsigned decisionBit(runtime::DecisionKind kind);

inline constexpr std::string_view kJoinEvent = "player.join";
inline constexpr std::string_view kServerChangeEvent = "player.server_change";
inline constexpr std::string_view kQuitEvent = "player.quit";
inline constexpr std::string_view kChatEvent = "player.chat";
inline constexpr std::string_view kKickEvent = "player.kick";
inline constexpr std::string_view kPingEvent = "server.ping";
inline constexpr std::string_view kLoginEvent = "player.login";
inline constexpr std::string_view kCommandEvent = "player.command";
inline constexpr std::string_view kPreConnectEvent = "player.server_pre_connect";

} // namespace basalt::proxy::plugins
