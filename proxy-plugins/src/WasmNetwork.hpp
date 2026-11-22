#pragma once

#include "WasmHandles.hpp"
#include "WasmValues.hpp"

#include <basalt/wire/plugins/PluginPackets.hpp>

namespace basalt::proxy::plugins {

inline constexpr std::string_view kNetworkArea = "proxy_network";
inline constexpr std::size_t kMaxChannelLength = 128;
inline constexpr std::size_t kMaxChannelsPerPlugin = 64;
inline constexpr std::size_t kMaxNetworkTokens = 64;
inline constexpr std::size_t kMaxInjectedPackets = 64;
inline constexpr std::size_t kMaxPluginMessage = 32768;

/// The packet token the registry carries: the observer bit above the plugin's own 32-bit token.
[[nodiscard]] inline std::int64_t packPacketToken(std::int32_t token, bool observer) {
    return (static_cast<std::int64_t>(observer) << 32U) | static_cast<std::uint32_t>(token);
}

/// What the callback in progress answers; the context handle is valid only while `call` is the host's live call.
struct NetworkLive {
    enum class Kind : std::uint8_t { kNone, kMessage, kPacket, kAntibot };

    Kind kind = Kind::kNone;
    const WasmCall* call = nullptr;
    wire::wasm::Value fields;
    wire::plugins::PluginPacket* packet = nullptr;
    bool observer = false;
    std::size_t injected = 0;
};

struct MessageHandler {
    std::string channel;
    std::int32_t token = 0;
};

/// Everything the network area keeps for one plugin; touched only while the plugin runs or holds the turn.
struct NetworkState {
    std::vector<std::string> channels;
    std::vector<MessageHandler> messageHandlers;
    std::vector<std::int32_t> antibotTokens;
    HandleTable<wire::plugins::PluginHandle> listeners;
    NetworkLive live;
};

[[nodiscard]] inline NetworkState& networkOf(WasmPlugin& plugin) {
    return areaState<NetworkState>(plugin, kNetworkArea);
}

/// kOk with `out` set when ctx is the live callback of this kind; STALE after it returned, INVALID for another ctx.
[[nodiscard]] inline std::int32_t liveContext(WasmPlugin& plugin, std::int32_t ctx, NetworkLive::Kind kind,
                                              NetworkLive*& out) {
    const WasmCall* live = plugin.host->live(ctx);
    if (live == nullptr)
        return ctx >= 1 && static_cast<std::uint32_t>(ctx) < plugin.host->nextCall_ ? wire::wasm::kErrStale
                                                                                    : wire::wasm::kErrInvalid;
    NetworkLive& state = networkOf(plugin).live;
    if (state.call != live || state.kind != kind)
        return wire::wasm::kErrInvalid;
    out = &state;
    return wire::wasm::kOk;
}

} // namespace basalt::proxy::plugins
