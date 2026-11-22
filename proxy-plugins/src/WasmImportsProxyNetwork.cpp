#include "WasmImports.hpp"
#include "WasmNetwork.hpp"

#include <basalt/proxy/config/AddressRange.hpp>
#include <basalt/proxy/plugins/BungeeMessaging.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>

#include <algorithm>

namespace basalt::proxy::plugins::wasmimport {
namespace {

using namespace wire::wasm;

constexpr std::string_view kNetwork = "network";

[[nodiscard]] bool validChannel(std::string_view channel) {
    return !channel.empty() && channel.size() <= kMaxChannelLength &&
           std::ranges::all_of(channel, [](unsigned char c) { return c > 32 && c < 127; });
}

[[nodiscard]] bool denied(WasmPlugin& plugin) { return !plugin.capable(kNetwork); }

// ---- channels

void channelRegister(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto name = text(call, 0);
    if (denied(plugin))
        return call.ret(kErrDenied);
    if (!name || !validChannel(*name) || isBungeeChannel(*name))
        return call.ret(kErrInvalid);
    NetworkState& state = networkOf(plugin);
    if (std::ranges::find(state.channels, *name) == state.channels.end()) {
        if (state.channels.size() >= kMaxChannelsPerPlugin)
            return call.ret(kErrInvalid);
        state.channels.emplace_back(*name);
    }
    plugin.host->announceChannel(*name, true);
    call.ret(kOk);
}

void channelUnregister(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto name = text(call, 0);
    if (denied(plugin))
        return call.ret(kErrDenied);
    auto& channels = networkOf(plugin).channels;
    const auto found = name ? std::ranges::find(channels, *name) : channels.end();
    if (found == channels.end())
        return call.ret(kErrAbsent);
    const std::string removed = *found;
    channels.erase(found);
    plugin.host->announceChannel(removed, false);
    call.ret(kOk);
}

void messageOn(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto filter = text(call, 0);
    if (denied(plugin))
        return call.ret(kErrDenied);
    auto& handlers = networkOf(plugin).messageHandlers;
    if (!filter || (!filter->empty() && !validChannel(*filter)) || handlers.size() >= kMaxNetworkTokens)
        return call.ret(kErrInvalid);
    handlers.push_back({std::string{*filter}, call.i32(2)});
    call.ret(kOk);
}

void messageGet(void* user, Call& call) {
    NetworkLive* live = nullptr;
    if (const std::int32_t status = liveContext(self(user), call.i32(0), NetworkLive::Kind::kMessage, live);
        status != kOk)
        return call.ret(status);
    call.ret(putValue(call.memory(), call.i32(1), call.i32(2), live->fields));
}

void messageSend(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto channel = text(call, 1);
    const auto data = call.memory().string(call.i32(3), call.i32(4));
    if (denied(plugin))
        return call.ret(kErrDenied);
    const WasmHost::Player* player = plugin.host->playerOf(call.i32(0));
    if (player == nullptr)
        return call.ret(kErrStale);
    const std::int32_t direction = call.i32(5);
    if (!channel || !data || !validChannel(*channel) || isBungeeChannel(*channel) || data->size() > kMaxPluginMessage ||
        (direction != 0 && direction != 1))
        return call.ret(kErrInvalid);
    const auto& send = plugin.host->hooks().sendMessage;
    const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(data->data()), data->size()};
    call.ret(send && send(player->name, *channel, bytes, direction == 0) ? kOk : kErrAbsent);
}

// ---- antibot

[[nodiscard]] std::int32_t antibotName(WasmPlugin& plugin, Call& call, std::string_view& name) {
    if (denied(plugin))
        return kErrDenied;
    const auto text = wasmimport::text(call, 0);
    if (!text || !wire::auth::validLoginName(*text))
        return kErrInvalid;
    name = *text;
    return kOk;
}

void antibotVerify(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    std::string_view name;
    if (const std::int32_t status = antibotName(plugin, call, name); status != kOk)
        return call.ret(status);
    const auto& hook = plugin.host->hooks().antibotVerify;
    if (!hook)
        return call.ret(kErrAbsent);
    hook(name);
    call.ret(kOk);
}

void antibotUnverify(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    std::string_view name;
    if (const std::int32_t status = antibotName(plugin, call, name); status != kOk)
        return call.ret(status);
    const auto& hook = plugin.host->hooks().antibotUnverify;
    if (!hook)
        return call.ret(kErrAbsent);
    hook(name);
    call.ret(kOk);
}

void antibotIsVerified(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    std::string_view name;
    if (const std::int32_t status = antibotName(plugin, call, name); status != kOk)
        return call.ret(status);
    const auto& hook = plugin.host->hooks().antibotVerified;
    call.ret(hook && hook(name) ? 1 : 0);
}

void antibotBan(WasmPlugin& plugin, Call& call, bool ban) {
    const auto address = text(call, 0);
    if (denied(plugin))
        return call.ret(kErrDenied);
    if (!address || !config::AddressRange::parseAddress(*address))
        return call.ret(kErrInvalid);
    const auto& hook = plugin.host->hooks().antibotBan;
    call.ret(hook && hook(*address, ban) ? kOk : kErrAbsent);
}

void antibotBanCall(void* user, Call& call) { antibotBan(self(user), call, true); }

void antibotUnbanCall(void* user, Call& call) { antibotBan(self(user), call, false); }

void antibotUnderAttack(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    if (denied(plugin))
        return call.ret(kErrDenied);
    const auto& hook = plugin.host->hooks().antibotAttack;
    call.ret(hook && hook() ? 1 : 0);
}

void antibotOn(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    if (denied(plugin))
        return call.ret(kErrDenied);
    auto& tokens = networkOf(plugin).antibotTokens;
    if (tokens.size() >= kMaxNetworkTokens)
        return call.ret(kErrInvalid);
    tokens.push_back(call.i32(0));
    call.ret(kOk);
}

void antibotEventGet(void* user, Call& call) {
    NetworkLive* live = nullptr;
    if (const std::int32_t status = liveContext(self(user), call.i32(0), NetworkLive::Kind::kAntibot, live);
        status != kOk)
        return call.ret(status);
    call.ret(putValue(call.memory(), call.i32(1), call.i32(2), live->fields));
}

} // namespace

void addProxyNetworkImports(std::vector<Entry>& out) {
    out.push_back({kProxyModule, "network_channel_register", channelRegister});
    out.push_back({kProxyModule, "network_channel_unregister", channelUnregister});
    out.push_back({kProxyModule, "network_message_on", messageOn});
    out.push_back({kProxyModule, "network_message_get", messageGet});
    out.push_back({kProxyModule, "network_message_send", messageSend});
    out.push_back({kProxyModule, "antibot_verify", antibotVerify});
    out.push_back({kProxyModule, "antibot_unverify", antibotUnverify});
    out.push_back({kProxyModule, "antibot_is_verified", antibotIsVerified});
    out.push_back({kProxyModule, "antibot_ban", antibotBanCall});
    out.push_back({kProxyModule, "antibot_unban", antibotUnbanCall});
    out.push_back({kProxyModule, "antibot_under_attack", antibotUnderAttack});
    out.push_back({kProxyModule, "antibot_on", antibotOn});
    out.push_back({kProxyModule, "antibot_event_get", antibotEventGet});
}

} // namespace basalt::proxy::plugins::wasmimport
