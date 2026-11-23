#include "WasmImports.hpp"
#include "WasmNetwork.hpp"

#include <basalt/wire/plugins/EventPriority.hpp>
#include <basalt/wire/plugins/PacketNaming.hpp>
#include <basalt/wire/plugins/PacketRegistry.hpp>

namespace basalt::proxy::plugins::wasmimport {
namespace {

using namespace wire::wasm;
using wire::plugins::PacketAddress;
using wire::plugins::PacketPhase;
using wire::plugins::PluginPacket;

constexpr std::string_view kPackets = "packets";

constexpr std::array<std::string_view, 5> kPhases{"handshake", "status", "login", "configuration", "play"};

[[nodiscard]] std::string_view phaseName(PacketPhase phase) { return kPhases[static_cast<std::size_t>(phase)]; }

[[nodiscard]] bool denied(WasmPlugin& plugin) { return !plugin.permitted(kPackets); }

[[nodiscard]] std::span<const std::uint8_t> bytesOf(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

[[nodiscard]] const std::vector<std::uint8_t>& currentBody(const PluginPacket& packet) {
    return packet.replacement ? *packet.replacement : packet.bytes;
}

void resolveCall(void*, Call& call) {
    const auto name = text(call, 0);
    const auto address = name ? wire::plugins::packetNaming::resolve(*name) : std::nullopt;
    if (!address)
        return call.ret(kErrAbsent);
    call.ret(putValue(call.memory(), call.i32(2), call.i32(3),
                      Value{Value::Map{
                          MapEntry{"name", Value{wire::plugins::packetNaming::of(*address)}},
                          MapEntry{"serverbound", Value{address->serverbound}},
                          MapEntry{"phase", Value{std::string{phaseName(address->phase)}}},
                          MapEntry{"id", Value{address->id}},
                      }}));
}

void namesCall(void*, Call& call) {
    Value::List names;
    for (std::string& name : wire::plugins::packetNaming::all())
        names.emplace_back(std::move(name));
    call.ret(putValue(call.memory(), call.i32(0), call.i32(1), Value{std::move(names)}));
}

struct ListenerOptions {
    std::int32_t priority = 2;
    bool failClosed = false;
};

[[nodiscard]] std::optional<ListenerOptions> optionsOf(const GuestMemory& memory, std::int32_t ptr, std::int32_t len) {
    ListenerOptions options;
    if (len == 0)
        return options;
    const auto value = readValue(memory, ptr, len);
    if (!value || value->asMap() == nullptr)
        return std::nullopt;
    if (const Value* priority = value->find("priority")) {
        if (const auto* number = priority->asInt()) {
            if (*number < 0 || *number > 4)
                return std::nullopt;
            options.priority = static_cast<std::int32_t>(*number);
        } else if (const auto* word = priority->asString()) {
            const auto spelled = wire::plugins::priorityOf(*word);
            if (!spelled)
                return std::nullopt;
            options.priority = static_cast<std::int32_t>(*spelled);
        } else {
            return std::nullopt;
        }
    }
    if (const Value* fail = value->find("fail")) {
        const auto* word = fail->asString();
        if (word == nullptr || (*word != "open" && *word != "closed"))
            return std::nullopt;
        options.failClosed = *word == "closed";
    }
    return options;
}

void listen(void* user, Call& call, bool observer) {
    WasmPlugin& plugin = self(user);
    const auto name = text(call, 0);
    if (denied(plugin))
        return call.ret(kErrDenied);
    wire::plugins::PacketRegistry* registry = plugin.host->packetRegistry();
    if (registry == nullptr)
        return call.ret(kErrAbsent);
    const auto scope = name ? wire::plugins::packetNaming::scopeOf(*name) : std::nullopt;
    const auto options = optionsOf(call.memory(), call.i32(2), call.i32(3));
    if (!scope || !options)
        return call.ret(kErrInvalid);
    auto handle = registry->add(&plugin, *scope, packPacketToken(call.i32(4), observer), options->priority, observer,
                                options->failClosed);
    const std::int32_t listener = networkOf(plugin).listeners.add(handle);
    if (listener == 0)
        handle.remove();
    call.ret(listener == 0 ? kErrInvalid : listener);
}

void packetOn(void* user, Call& call) { listen(user, call, false); }

void packetWatch(void* user, Call& call) { listen(user, call, true); }

void packetRemove(void* user, Call& call) {
    NetworkState& state = networkOf(self(user));
    wire::plugins::PluginHandle* handle = nullptr;
    if (state.listeners.find(call.i32(0), handle) != kOk)
        return call.ret(kErrAbsent);
    handle->remove();
    static_cast<void>(state.listeners.remove(call.i32(0)));
    call.ret(kOk);
}

[[nodiscard]] std::int32_t livePacket(void* user, Call& call, NetworkLive*& live) {
    return liveContext(self(user), call.i32(0), NetworkLive::Kind::kPacket, live);
}

void packetGet(void* user, Call& call) {
    NetworkLive* live = nullptr;
    if (const std::int32_t status = livePacket(user, call, live); status != kOk)
        return call.ret(status);
    const PluginPacket& packet = *live->packet;
    call.ret(putValue(call.memory(), call.i32(1), call.i32(2),
                      Value{Value::Map{
                          MapEntry{"name", Value{packet.name}},
                          MapEntry{"serverbound", Value{packet.address.serverbound}},
                          MapEntry{"phase", Value{std::string{phaseName(packet.address.phase)}}},
                          MapEntry{"id", Value{packet.address.id}},
                          MapEntry{"connection", Value{static_cast<std::int64_t>(packet.connection)}},
                          MapEntry{"protocol", Value{packet.clientProtocol}},
                          MapEntry{"backend_protocol", Value{packet.backendProtocol}},
                          MapEntry{"translated", Value{packet.translated}},
                          MapEntry{"size", Value{static_cast<std::int64_t>(currentBody(packet).size())}},
                          MapEntry{"backend", Value{packet.backend}},
                          MapEntry{"player_name", Value{packet.playerName}},
                          MapEntry{"player_uuid", Value{packet.playerUuid}},
                          MapEntry{"editable", Value{packet.editable}},
                          MapEntry{"observer", Value{live->observer}},
                          MapEntry{"cancelled", Value{packet.cancelled}},
                      }}));
}

void packetBody(void* user, Call& call) {
    NetworkLive* live = nullptr;
    if (const std::int32_t status = livePacket(user, call, live); status != kOk)
        return call.ret(status);
    const auto& body = currentBody(*live->packet);
    call.ret(call.memory().put(call.i32(1), call.i32(2),
                               std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}));
}

[[nodiscard]] std::int32_t editable(void* user, Call& call, NetworkLive*& live) {
    if (const std::int32_t status = livePacket(user, call, live); status != kOk)
        return status;
    return live->observer || !live->packet->editable ? kErrInvalid : kOk;
}

void packetSetBody(void* user, Call& call) {
    NetworkLive* live = nullptr;
    if (const std::int32_t status = editable(user, call, live); status != kOk)
        return call.ret(status);
    const auto body = call.memory().string(call.i32(1), call.i32(2));
    if (!body || body->size() > self(user).host->packetMaxBytes())
        return call.ret(kErrInvalid);
    const auto bytes = bytesOf(*body);
    live->packet->replacement = std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    call.ret(kOk);
}

void packetCancel(void* user, Call& call) {
    NetworkLive* live = nullptr;
    if (const std::int32_t status = editable(user, call, live); status != kOk)
        return call.ret(status);
    live->packet->cancelled = true;
    call.ret(kOk);
}

void packetInject(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    NetworkLive* live = nullptr;
    if (const std::int32_t status = livePacket(user, call, live); status != kOk)
        return call.ret(status);
    if (denied(plugin))
        return call.ret(kErrDenied);
    const auto name = text(call, 1);
    const auto body = call.memory().string(call.i32(3), call.i32(4));
    const auto address = name ? wire::plugins::packetNaming::resolve(*name) : std::nullopt;
    PluginPacket& packet = *live->packet;
    if (!address || !body || address->phase != packet.address.phase || body->size() > plugin.host->packetMaxBytes() ||
        live->injected >= kMaxInjectedPackets)
        return call.ret(kErrInvalid);
    if (packet.endpoint == nullptr)
        return call.ret(kErrAbsent);
    ++live->injected;
    const bool intercept = call.i32(5) != 0;
    const bool handed = address->serverbound
                            ? packet.endpoint->receiveServerbound(address->id, bytesOf(*body), intercept)
                            : packet.endpoint->sendClientbound(address->id, bytesOf(*body), intercept);
    call.ret(handed ? kOk : kErrAbsent);
}

void toPlayer(void* user, Call& call, bool serverbound) {
    WasmPlugin& plugin = self(user);
    const auto name = text(call, 1);
    const auto body = call.memory().string(call.i32(3), call.i32(4));
    if (denied(plugin))
        return call.ret(kErrDenied);
    const WasmHost::Player* player = plugin.host->playerOf(call.i32(0));
    if (player == nullptr)
        return call.ret(kErrStale);
    const auto address = name ? wire::plugins::packetNaming::resolve(*name) : std::nullopt;
    if (!address || address->serverbound != serverbound || !body || body->size() > plugin.host->packetMaxBytes())
        return call.ret(kErrInvalid);
    const auto& send = plugin.host->hooks().packet;
    call.ret(
        send && send(player->name, serverbound, static_cast<std::int32_t>(address->phase), address->id, bytesOf(*body))
            ? kOk
            : kErrAbsent);
}

void packetSend(void* user, Call& call) { toPlayer(user, call, false); }

void packetReceive(void* user, Call& call) { toPlayer(user, call, true); }

} // namespace

void addProxyNetworkExtraImports(std::vector<Entry>& out) {
    out.push_back({kProxyModule, "packet_resolve", resolveCall});
    out.push_back({kProxyModule, "packet_names", namesCall});
    out.push_back({kProxyModule, "packet_on", packetOn});
    out.push_back({kProxyModule, "packet_watch", packetWatch});
    out.push_back({kProxyModule, "packet_remove", packetRemove});
    out.push_back({kProxyModule, "packet_get", packetGet});
    out.push_back({kProxyModule, "packet_body", packetBody});
    out.push_back({kProxyModule, "packet_set_body", packetSetBody});
    out.push_back({kProxyModule, "packet_cancel", packetCancel});
    out.push_back({kProxyModule, "packet_inject", packetInject});
    out.push_back({kProxyModule, "packet_send", packetSend});
    out.push_back({kProxyModule, "packet_receive", packetReceive});
}

} // namespace basalt::proxy::plugins::wasmimport
