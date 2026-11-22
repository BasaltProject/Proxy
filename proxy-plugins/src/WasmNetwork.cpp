#include "WasmNetwork.hpp"

#include <algorithm>

namespace basalt::proxy::plugins {
namespace {

using wire::wasm::Blob;
using wire::wasm::MapEntry;
using wire::wasm::Value;

[[nodiscard]] std::int64_t numberOf(std::string_view text) {
    std::int64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return 0;
        value = value * 10 + (c - '0');
    }
    return value;
}

} // namespace

void WasmHost::announceChannel(std::string_view channel, bool wanted) {
    const std::string name{channel};
    const bool stillWanted = std::ranges::any_of(plugins_, [&](const auto& other) {
        const auto& have = networkOf(*other).channels;
        return std::ranges::find(have, name) != have.end();
    });
    std::unique_lock lock{host_.channelsMutex_};
    if (wanted)
        host_.channels_.insert(name);
    else if (!stillWanted)
        host_.channels_.erase(name);
}

void WasmHost::deliverMessage(std::string_view player, std::string_view channel, std::span<const std::uint8_t> data,
                              bool fromBackend) {
    const PluginHost::Player* known = host_.findPlayer(player);
    const std::string_view name = known != nullptr ? std::string_view{known->name} : player;
    const std::string_view uuid = known != nullptr ? std::string_view{known->uuid} : std::string_view{};
    const std::string_view server = known != nullptr ? std::string_view{known->server} : std::string_view{};
    for (auto& plugin : plugins_) {
        NetworkState& state = networkOf(*plugin);
        if (plugin->broken || std::ranges::find(state.channels, channel) == state.channels.end())
            continue;
        for (const MessageHandler& handler : state.messageHandlers) {
            if (!handler.channel.empty() && handler.channel != channel)
                continue;
            WasmCall call;
            call.name = name;
            call.uuid = uuid;
            call.server = server;
            state.live = NetworkLive{.kind = NetworkLive::Kind::kMessage, .call = &call};
            state.live.fields = Value{Value::Map{
                MapEntry{"player_name", Value{std::string{name}}},
                MapEntry{"player_uuid", Value{std::string{uuid}}},
                MapEntry{"channel", Value{std::string{channel}}},
                MapEntry{"data", Value{Blob{std::vector<std::uint8_t>(data.begin(), data.end())}}},
                MapEntry{"source", Value{fromBackend ? "backend" : "client"}},
                MapEntry{"server", Value{std::string{server}}},
            }};
            const bool ran =
                dispatch(*plugin, handler.token, &call, "message " + std::string{channel}, kWasmCallBudget);
            state.live = NetworkLive{};
            if (!ran && plugin->broken)
                break;
        }
    }
}

void WasmHost::antibotEvent(const runtime::PlayerEvent& event) {
    const std::string_view packed = event.uuid;
    const auto first = packed.find(':');
    const auto second = first == std::string_view::npos ? first : packed.find(':', first + 1);
    const std::string action{packed.substr(0, first)};
    const std::string reason{first == std::string_view::npos ? std::string_view{}
                                                             : packed.substr(first + 1, second - first - 1)};
    const std::int64_t skipped = second == std::string_view::npos ? 0 : numberOf(packed.substr(second + 1));
    for (auto& plugin : plugins_) {
        NetworkState& state = networkOf(*plugin);
        for (const std::int32_t token : state.antibotTokens) {
            if (plugin->broken)
                break;
            WasmCall call;
            call.name = event.player;
            state.live = NetworkLive{.kind = NetworkLive::Kind::kAntibot, .call = &call};
            state.live.fields = Value{Value::Map{
                MapEntry{"kind", Value{event.backend}},
                MapEntry{"player_name", Value{event.player}},
                MapEntry{"action", Value{action}},
                MapEntry{"reason", Value{reason}},
                MapEntry{"skipped", Value{skipped}},
                MapEntry{"origin", Value{event.from}},
            }};
            dispatch(*plugin, token, &call, "antibot event", kWasmCallBudget);
            state.live = NetworkLive{};
        }
    }
}

bool WasmHost::deliverPacket(const void* owner, std::int64_t packed, wire::plugins::PluginPacket& packet) {
    if (turnHolder_.load() == std::this_thread::get_id())
        return true;
    std::unique_lock<std::recursive_timed_mutex> turn{turn_, std::defer_lock};
    const std::chrono::milliseconds budget = host_.settings_.packetTimeout;
    if (!turn.try_lock_for(budget)) {
        const std::size_t waited = host_.packetTimeouts_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (host_.hooks_.log && (waited == 1 || waited % 1000 == 0))
            host_.hooks_.log("plugins", "packet listeners waited over " + std::to_string(budget.count()) +
                                            " ms for the plugin thread: " + std::to_string(waited) +
                                            " packet(s) so far went on untouched or were cancelled by fail-closed "
                                            "listeners");
        return false;
    }
    WasmPlugin& plugin = *static_cast<WasmPlugin*>(const_cast<void*>(owner));
    if (plugin.broken)
        return true;
    NetworkState& state = networkOf(plugin);
    WasmCall call;
    call.name = packet.playerName;
    call.uuid = packet.playerUuid;
    call.server = packet.backend;
    state.live = NetworkLive{
        .kind = NetworkLive::Kind::kPacket, .call = &call, .packet = &packet, .observer = (packed >> 32U) != 0};
    dispatch(plugin, static_cast<std::int32_t>(packed & 0xFFFFFFFF), &call, "packet " + packet.name, budget);
    state.live = NetworkLive{};
    return true;
}

void WasmHost::dropPacketListeners() {
    for (auto& plugin : plugins_) {
        NetworkState& state = networkOf(*plugin);
        state.listeners.forEach([](std::int32_t, wire::plugins::PluginHandle& handle) { handle.remove(); });
        state.listeners.clear();
    }
}

} // namespace basalt::proxy::plugins
