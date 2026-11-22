#pragma once

#include "WasmValues.hpp"

#include <basalt/wire/text/Component.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace basalt::proxy::plugins {

inline constexpr std::string_view kInteractionArea = "proxy_interaction";

struct InteractionState {
    std::vector<std::int32_t> packWatchers;
    std::vector<std::int32_t> settingsWatchers;
    std::uint64_t serial = 0;
};

[[nodiscard]] InteractionState& interactionState(WasmPlugin& plugin);

/// Tells every observer of player.settings; `player` is the roster entry with the new settings.
void notifySettings(WasmHost& host, const WasmHost::Player& player);

/// Tells every observer of player.resource_pack; `status` indexes the eight status words of the protocol.
void notifyResourcePack(WasmHost& host, std::string_view player, std::string_view id, std::int64_t status);

[[nodiscard]] wire::wasm::Value settingsValue(const runtime::ClientSettings& settings);

namespace wasmimport {
/// Text of a message argument as a component: a leading '@' is a lang key of the plugin, cut at kWasmMaxText.
[[nodiscard]] wire::text::Component componentOf(const WasmPlugin& plugin, std::string_view line);
} // namespace wasmimport

} // namespace basalt::proxy::plugins
