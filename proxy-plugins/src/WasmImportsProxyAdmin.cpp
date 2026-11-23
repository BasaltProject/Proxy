#include "WasmAdminState.hpp"
#include "WasmImports.hpp"
#include "WasmValues.hpp"

#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/storage/PluginDataRepository.hpp>
#include <basalt/wire/text/ComponentJson.hpp>
#include <basalt/wire/text/Serializers.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace basalt::proxy::plugins::wasmimport {
namespace {

using namespace wire::wasm;

constexpr std::size_t kMaxKeyBytes = 128;
constexpr std::size_t kMaxStoredBytes = 65536;
constexpr std::size_t kMaxJsonBytes = 65536;
constexpr std::size_t kMaxHolderBytes = 64;
constexpr std::int32_t kDefaultTop = 10;
constexpr std::int32_t kMaxTop = 1000;
constexpr std::string_view kServerScope = "server";

[[nodiscard]] bool pluginMayRun(const WasmPlugin& plugin) { return plugin.permitted("commands"); }

void writeReason(Call& call, std::size_t ptrIndex, std::string_view reason) {
    const std::int32_t cap = call.i32(ptrIndex + 1);
    if (cap <= 0)
        return;
    const std::string_view cut = reason.substr(0, static_cast<std::size_t>(cap));
    static_cast<void>(call.memory().put(call.i32(ptrIndex), cap, cut));
}

// ---- backends

void serversRegister(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const auto name = text(call, 0);
    const auto host = text(call, 2);
    const auto like = text(call, 5);
    if (!name || !host || !like || name->empty() || host->empty())
        return call.ret(kErrInvalid);
    if (call.i32(4) < 1 || call.i32(4) > 65535) {
        writeReason(call, 7, "the port is from 1 to 65535");
        return call.ret(kErrInvalid);
    }
    if (!wasm.hooks().registerServer) {
        writeReason(call, 7, "servers cannot be registered here");
        return call.ret(kErrInvalid);
    }
    const auto error = wasm.hooks().registerServer(*name, *host, static_cast<std::uint16_t>(call.i32(4)), *like);
    if (error)
        writeReason(call, 7, *error);
    call.ret(error ? kErrInvalid : kOk);
}

void serversUnregister(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const auto name = text(call, 0);
    if (!name)
        return call.ret(kErrInvalid);
    if (wasm.hooks().unregisterServer && wasm.hooks().unregisterServer(*name))
        return call.ret(kOk);
    const bool listed = wasm.hooks().servers && std::ranges::contains(wasm.hooks().servers(), *name);
    call.ret(listed ? kErrDenied : kErrAbsent);
}

[[nodiscard]] Value pingValue(const std::optional<runtime::ServerPing>& answer) {
    if (!answer)
        return Value{};
    const auto& status = answer->status;
    Value::Map map;
    map.push_back({"motd", Value{wire::text::toPlainText(status.description)}});
    map.push_back(
        {"motd_json", Value{wire::common::writeJson(wire::text::componentJson::serialize(status.description))}});
    map.push_back({"online", Value{std::int64_t{status.players ? status.players->online : 0}}});
    map.push_back({"max_players", Value{std::int64_t{status.players ? status.players->max : 0}}});
    map.push_back({"version", Value{status.version ? status.version->name : std::string{}}});
    map.push_back({"protocol", Value{std::int64_t{status.version ? status.version->protocol : 0}}});
    map.push_back({"latency", Value{std::int64_t{answer->latency.count()}}});
    return Value{std::move(map)};
}

void serversPing(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    PluginHost* host = &plugin.host->host();
    const auto name = text(call, 0);
    if (!name || name->empty() || !plugin.host->hooks().pingServer)
        return call.ret(name && !name->empty() ? kErrAbsent : kErrInvalid);
    const std::int32_t request = beginRequest(plugin, call.i32(2));
    if (request == 0)
        return call.ret(kErrInvalid);
    const auto reply = [host, life = std::weak_ptr<WasmPlugin*>{adminState(plugin).life},
                        request](std::optional<runtime::ServerPing> answer) {
        auto value = std::make_shared<Value>(pingValue(answer));
        static_cast<void>(host->runOnPluginThread([life, request, value] {
            if (const auto alive = life.lock(); alive && *alive != nullptr)
                static_cast<void>(completeRequest(**alive, request, *value));
        }));
    };
    if (!plugin.host->hooks().pingServer(*name, reply)) {
        static_cast<void>(requests(plugin).remove(request));
        return call.ret(kErrAbsent);
    }
    call.ret(request);
}

// ---- console and chat

void consoleRun(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto line = text(call, 0);
    if (!pluginMayRun(plugin))
        return call.ret(kErrDenied);
    if (!line || !plugin.host->hooks().console)
        return call.ret(kErrInvalid);
    const std::string_view cut = line->substr(0, kWasmMaxText);
    plugin.host->logLine(plugin, "", "ran /" + std::string{cut});
    call.ret(plugin.host->hooks().console(cut) ? 1 : 0);
}

void playerChat(void* user, Call& call) {
    WasmHost& wasm = *self(user).host;
    const WasmHost::Player* player = wasm.playerOf(call.i32(0));
    const auto line = text(call, 1);
    if (player == nullptr)
        return call.ret(kErrStale);
    if (!line || line->empty() || !wasm.hooks().spoofChat)
        return call.ret(kErrInvalid);
    call.ret(wasm.hooks().spoofChat(player->name, line->substr(0, kWasmMaxText)) ? kOk : kErrInvalid);
}

// ---- component JSON

void playerJson(void* user, Call& call, bool kick) {
    WasmHost& wasm = *self(user).host;
    const WasmHost::Player* player = wasm.playerOf(call.i32(0));
    const auto json = text(call, 1);
    if (player == nullptr)
        return call.ret(kErrStale);
    if (!json || json->size() > kMaxJsonBytes)
        return call.ret(kErrInvalid);
    auto component = componentOfJson(*json);
    if (!component)
        return call.ret(kErrInvalid);
    const auto& hook = kick ? wasm.hooks().kickComponent : wasm.hooks().tellComponent;
    if (!hook)
        return call.ret(kErrInvalid);
    hook(player->name, std::move(*component));
    call.ret(kOk);
}

void playerSendJson(void* user, Call& call) { playerJson(user, call, false); }

void playerKickJson(void* user, Call& call) { playerJson(user, call, true); }

// ---- store and per-plugin data

[[nodiscard]] std::optional<std::string> encodeStored(const Value& value) {
    if (const bool* flag = value.asBool())
        return *flag ? "b1" : "b0";
    if (const std::int64_t* whole = value.asInt())
        return "n" + std::to_string(*whole);
    if (const double* real = value.asFloat()) {
        if (!std::isfinite(*real))
            return std::nullopt;
        std::array<char, 40> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *real);
        return "n" + std::string{buffer.data(), end};
    }
    if (const std::string* line = value.asString())
        return "s" + *line;
    return std::nullopt;
}

[[nodiscard]] Value decodeStored(std::string_view stored) {
    if (stored.empty())
        return Value{};
    if (stored[0] == 's')
        return Value{std::string{stored.substr(1)}};
    if (stored[0] == 'b')
        return Value{stored.size() > 1 && stored[1] == '1'};
    const std::string_view number = stored.substr(1);
    std::int64_t whole = 0;
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), whole);
    if (error == std::errc{} && end == number.data() + number.size())
        return Value{whole};
    return Value{std::strtod(std::string{number}.c_str(), nullptr)};
}

struct Scope {
    wire::storage::PluginDataRepository repository;
    std::string holder;
    std::string key;
};

/// The scope to read or write, or the ABI result to answer: DENIED, or INVALID for a bad key, holder or no database.
[[nodiscard]] std::expected<Scope, std::int32_t> scopeOf(WasmPlugin& plugin, std::string_view capability,
                                                         std::string_view holder, std::string_view key) {
    if (!plugin.capable(capability))
        return std::unexpected(kErrDenied);
    wire::sql::Database* database = plugin.host->database();
    if (database == nullptr || key.empty() || key.size() > kMaxKeyBytes || holder.empty() ||
        holder.size() > kMaxHolderBytes)
        return std::unexpected(kErrInvalid);
    return Scope{wire::storage::PluginDataRepository{*database}, std::string{holder}, std::string{key}};
}

void getStored(WasmPlugin& plugin, Call& call, std::string_view capability, std::string_view holder,
               std::size_t keyIndex) {
    const auto key = text(call, keyIndex);
    if (!key)
        return call.ret(kErrInvalid);
    auto scope = scopeOf(plugin, capability, holder, *key);
    if (!scope)
        return call.ret(scope.error());
    const auto stored = scope->repository.get(plugin.id, scope->holder, scope->key);
    if (!stored)
        return call.ret(kErrAbsent);
    call.ret(putValue(call.memory(), call.i32(keyIndex + 2), call.i32(keyIndex + 3), decodeStored(*stored)));
}

void setStored(WasmPlugin& plugin, Call& call, std::string_view capability, std::string_view holder,
               std::size_t keyIndex) {
    const auto key = text(call, keyIndex);
    const auto value = readValue(call.memory(), call.i32(keyIndex + 2), call.i32(keyIndex + 3));
    if (!key || !value)
        return call.ret(kErrInvalid);
    auto scope = scopeOf(plugin, capability, holder, *key);
    if (!scope)
        return call.ret(scope.error());
    std::optional<std::string> encoded;
    if (!value->isNull()) {
        encoded = encodeStored(*value);
        if (!encoded || encoded->size() > kMaxStoredBytes)
            return call.ret(kErrInvalid);
        if (std::cmp_greater_equal(scope->repository.all(plugin.id, scope->holder).size(), kWasmMaxKeys) &&
            !scope->repository.get(plugin.id, scope->holder, scope->key))
            return call.ret(kErrInvalid);
    }
    scope->repository.set(plugin.id, scope->holder, scope->key, encoded);
    call.ret(kOk);
}

void storeGet(void* user, Call& call) { getStored(self(user), call, "storage", kServerScope, 0); }

void storeSet(void* user, Call& call) { setStored(self(user), call, "storage", kServerScope, 0); }

[[nodiscard]] std::string nameOfHolder(WasmHost& wasm, std::string_view uuid) {
    if (uuid == kServerScope)
        return std::string{kServerScope};
    for (const auto& [name, player] : wasm.roster())
        if (player.uuid == uuid)
            return name;
    return std::string{uuid};
}

void storeTop(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto key = text(call, 0);
    if (!key)
        return call.ret(kErrInvalid);
    auto scope = scopeOf(plugin, "storage", kServerScope, *key);
    if (!scope)
        return call.ret(scope.error());
    const std::int32_t limit = call.i32(2) < 1 ? kDefaultTop : std::min(call.i32(2), kMaxTop);
    Value::List rows;
    for (const auto& [uuid, stored] : scope->repository.top(plugin.id, *key, limit)) {
        Value::Map row;
        row.push_back({"uuid", Value{uuid}});
        row.push_back({"name", Value{nameOfHolder(*plugin.host, uuid)}});
        row.push_back({"value", decodeStored(stored)});
        rows.emplace_back(std::move(row));
    }
    call.ret(putValue(call.memory(), call.i32(3), call.i32(4), Value{std::move(rows)}));
}

void playerDataGet(void* user, Call& call) {
    const auto holder = text(call, 0);
    if (!holder || *holder == kServerScope)
        return call.ret(kErrInvalid);
    getStored(self(user), call, "players_data", *holder, 2);
}

void playerDataSet(void* user, Call& call) {
    const auto holder = text(call, 0);
    if (!holder || *holder == kServerScope)
        return call.ret(kErrInvalid);
    setStored(self(user), call, "players_data", *holder, 2);
}

// ---- waiting

void wait(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    WasmHost& wasm = *plugin.host;
    if (call.i32(0) < 0)
        return call.ret(kErrInvalid);
    const std::int32_t request = beginRequest(plugin, call.i32(1));
    if (request == 0)
        return call.ret(kErrInvalid);
    wasm.timers_.push_back(WasmTimer{&plugin,
                                     wasm.nextTimer_++,
                                     0,
                                     std::chrono::steady_clock::now() + kTick * std::max(call.i32(0), 1),
                                     {},
                                     request});
    call.ret(request);
}

constexpr std::array kAdmin = std::to_array<Entry>({
    {kProxyModule, "servers_register", serversRegister},
    {kProxyModule, "servers_unregister", serversUnregister},
    {kProxyModule, "servers_ping", serversPing},
    {kProxyModule, "console_run", consoleRun},
    {kProxyModule, "player_chat", playerChat},
    {kProxyModule, "player_send_json", playerSendJson},
    {kProxyModule, "player_kick_json", playerKickJson},
    {kProxyModule, "store_get", storeGet},
    {kProxyModule, "store_set", storeSet},
    {kProxyModule, "store_top", storeTop},
    {kProxyModule, "player_data_get", playerDataGet},
    {kProxyModule, "player_data_set", playerDataSet},
    {kProxyModule, "wait", wait},
});

} // namespace

void addProxyAdminImports(std::vector<Entry>& out) { out.insert(out.end(), kAdmin.begin(), kAdmin.end()); }

} // namespace basalt::proxy::plugins::wasmimport
