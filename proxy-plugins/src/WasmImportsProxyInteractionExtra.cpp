#include "WasmImports.hpp"
#include "WasmInteraction.hpp"

#include <basalt/wire/storage/PluginDataRepository.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace basalt::proxy::plugins::wasmimport {
namespace {

using namespace wire::wasm;

void playersByUuid(void* user, Call& call) {
    WasmHost& host = *self(user).host;
    const auto uuid = text(call, 0);
    if (!uuid)
        return call.ret(0);
    for (const auto& [name, player] : host.roster())
        if (!player.uuid.empty() && player.uuid == *uuid)
            return call.ret(host.handleOf(name));
    call.ret(0);
}

void playersCount(void* user, Call& call) {
    WasmHost& host = *self(user).host;
    const auto server = text(call, 0);
    if (!server || server->empty())
        return call.ret(static_cast<std::int32_t>(host.roster().size()));
    call.ret(static_cast<std::int32_t>(
        std::ranges::count_if(host.roster(), [&](const auto& entry) { return entry.second.server == *server; })));
}

void playersBroadcast(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto line = resolve(plugin, call, 0, 2);
    if (!line)
        return call.ret(kErrInvalid);
    if (plugin.host->hooks().tell)
        for (const auto& [name, player] : plugin.host->roster())
            plugin.host->hooks().tell(name, line->substr(0, kWasmMaxText));
    call.ret(kOk);
}

void playerAddress(void* user, Call& call) {
    const WasmHost::Player* player = self(user).host->playerOf(call.i32(0));
    if (player == nullptr)
        return call.ret(kErrStale);
    call.ret(player->address.empty() ? kErrAbsent : call.memory().put(call.i32(1), call.i32(2), player->address));
}

void playerPort(void* user, Call& call) {
    const WasmHost::Player* player = self(user).host->playerOf(call.i32(0));
    if (player == nullptr)
        return call.ret(kErrStale);
    call.ret(player->port == 0 ? kErrAbsent : static_cast<std::int32_t>(player->port));
}

void playerSettings(void* user, Call& call) {
    const WasmHost::Player* player = self(user).host->playerOf(call.i32(0));
    if (player == nullptr)
        return call.ret(kErrStale);
    if (!player->settings)
        return call.ret(kErrAbsent);
    call.ret(putValue(call.memory(), call.i32(1), call.i32(2), settingsValue(*player->settings)));
}

struct DataTarget {
    wire::storage::PluginDataRepository repository;
    std::string scope;
    std::string_view key;
};

[[nodiscard]] std::optional<DataTarget> dataTarget(WasmPlugin& plugin, Call& call) {
    wire::sql::Database* database = plugin.host->database();
    const WasmHost::Player* player = plugin.host->playerOf(call.i32(0));
    const auto key = text(call, 1);
    if (!plugin.capable("players_data") || database == nullptr || player == nullptr || !key)
        return std::nullopt;
    return DataTarget{wire::storage::PluginDataRepository{*database},
                      player->uuid.empty() ? player->name : player->uuid, *key};
}

void dataGetF64(void* user, Call& call) {
    auto target = dataTarget(self(user), call);
    const auto stored = target ? target->repository.get(self(user).id, target->scope, target->key) : std::nullopt;
    double number = call.f64(3);
    if (stored && stored->size() > 1 && (*stored)[0] == 'n')
        number = std::strtod(stored->c_str() + 1, nullptr);
    call.ret(number);
}

void dataSetF64(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    if (!plugin.capable("players_data"))
        return call.ret(kErrDenied);
    if (plugin.host->playerOf(call.i32(0)) == nullptr)
        return call.ret(kErrStale);
    auto target = dataTarget(plugin, call);
    if (!target || !std::isfinite(call.f64(3)))
        return call.ret(kErrInvalid);
    if (std::cmp_greater_equal(target->repository.all(plugin.id, target->scope).size(), kWasmMaxKeys) &&
        !target->repository.get(plugin.id, target->scope, target->key))
        return call.ret(kErrInvalid);
    std::array<char, 40> buffer{};
    const auto end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), call.f64(3)).ptr;
    target->repository.set(plugin.id, target->scope, target->key, "n" + std::string{buffer.data(), end});
    call.ret(kOk);
}

constexpr std::array kExtra = std::to_array<Entry>({
    {kProxyModule, "players_by_uuid", playersByUuid},
    {kProxyModule, "players_count", playersCount},
    {kProxyModule, "players_broadcast", playersBroadcast},
    {kProxyModule, "player_address", playerAddress},
    {kProxyModule, "player_port", playerPort},
    {kProxyModule, "player_settings", playerSettings},
    {kProxyModule, "player_data_get_f64", dataGetF64},
    {kProxyModule, "player_data_set_f64", dataSetF64},
});

} // namespace

void addProxyInteractionExtraImports(std::vector<Entry>& out) { out.insert(out.end(), kExtra.begin(), kExtra.end()); }

} // namespace basalt::proxy::plugins::wasmimport
