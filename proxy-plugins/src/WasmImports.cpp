#include "WasmImports.hpp"

#include <basalt/wire/storage/PluginDataRepository.hpp>
#include <basalt/wire/text/MiniMessage.hpp>

#include <algorithm>
#include <charconv>
#include <utility>

namespace basalt::proxy::plugins {
namespace wasmimport {
namespace {

using wire::common::toml::TomlTable;
using wire::common::toml::TomlValue;
using namespace wire::wasm;

[[nodiscard]] const TomlValue* lookup(const TomlTable& root, std::string_view key) {
    const TomlTable* table = &root;
    for (auto dot = key.find('.'); dot != std::string_view::npos; dot = key.find('.')) {
        table = table->table(key.substr(0, dot));
        if (table == nullptr)
            return nullptr;
        key.remove_prefix(dot + 1);
    }
    return table->find(key);
}

void log(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto line = text(call, 1);
    if (!line)
        return;
    constexpr std::array<std::string_view, 4> kPrefix{"debug: ", "", "warn: ", "error: "};
    plugin.host->logLine(plugin, kPrefix[static_cast<std::size_t>(std::clamp(call.i32(0), 0, 3))],
                         line->substr(0, kWasmMaxText));
}

void configI64(void* user, Call& call) {
    const auto key = text(call, 0);
    const TomlValue* value = key ? lookup(self(user).config, *key) : nullptr;
    const auto* whole = value != nullptr ? value->get<std::int64_t>() : nullptr;
    call.ret(whole != nullptr ? *whole : call.i64(2));
}

void configF64(void* user, Call& call) {
    const auto key = text(call, 0);
    const TomlValue* value = key ? lookup(self(user).config, *key) : nullptr;
    if (value != nullptr && value->get<double>() != nullptr)
        return call.ret(*value->get<double>());
    if (value != nullptr && value->get<std::int64_t>() != nullptr)
        return call.ret(static_cast<double>(*value->get<std::int64_t>()));
    call.ret(call.f64(2));
}

void configBool(void* user, Call& call) {
    const auto key = text(call, 0);
    const TomlValue* value = key ? lookup(self(user).config, *key) : nullptr;
    const auto* flag = value != nullptr ? value->get<bool>() : nullptr;
    call.ret(flag != nullptr ? static_cast<std::int32_t>(*flag) : call.i32(2));
}

void configStr(void* user, Call& call) {
    const auto key = text(call, 0);
    const TomlValue* value = key ? lookup(self(user).config, *key) : nullptr;
    const auto* string = value != nullptr ? value->get<std::string>() : nullptr;
    call.ret(string != nullptr ? call.memory().put(call.i32(2), call.i32(3), *string) : kErrAbsent);
}

void translate(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto key = text(call, 0);
    const auto args = call.memory().string(call.i32(2), call.i32(3));
    const auto line = key ? plugin.text->translation(*key) : std::nullopt;
    if (!key || !args || !line)
        return call.ret(kErrAbsent);
    call.ret(call.memory().put(call.i32(4), call.i32(5), applyArguments(*line, *args)));
}

[[nodiscard]] std::string playerScope(const WasmHost::Player& player) {
    return player.uuid.empty() ? player.name : player.uuid;
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
    return DataTarget{wire::storage::PluginDataRepository{*database}, playerScope(*player), *key};
}

[[nodiscard]] std::int32_t dataStatus(WasmPlugin& plugin, Call& call) {
    if (!plugin.capable("players_data"))
        return kErrDenied;
    if (plugin.host->playerOf(call.i32(0)) == nullptr)
        return kErrStale;
    return plugin.host->database() == nullptr || !text(call, 1) ? kErrInvalid : kOk;
}

[[nodiscard]] std::optional<std::string> stored(WasmPlugin& plugin, Call& call) {
    auto target = dataTarget(plugin, call);
    return target ? target->repository.get(plugin.id, target->scope, target->key) : std::nullopt;
}

void dataGetI64(void* user, Call& call) {
    const auto value = stored(self(user), call);
    std::int64_t whole = call.i64(3);
    if (value && value->size() > 1 && (*value)[0] == 'n')
        std::from_chars(value->data() + 1, value->data() + value->size(), whole);
    call.ret(whole);
}

void dataGetBool(void* user, Call& call) {
    const auto value = stored(self(user), call);
    call.ret(value && value->size() > 1 && (*value)[0] == 'b' ? static_cast<std::int32_t>((*value)[1] == '1')
                                                              : call.i32(3));
}

void dataGetStr(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    if (const std::int32_t status = dataStatus(plugin, call); status != kOk)
        return call.ret(status);
    const auto value = stored(plugin, call);
    if (!value || value->empty() || (*value)[0] != 's')
        return call.ret(kErrAbsent);
    call.ret(call.memory().put(call.i32(3), call.i32(4), std::string_view{*value}.substr(1)));
}

[[nodiscard]] std::int32_t store(WasmPlugin& plugin, Call& call, const std::optional<std::string>& value) {
    if (const std::int32_t status = dataStatus(plugin, call); status != kOk)
        return status;
    auto target = dataTarget(plugin, call);
    if (!target)
        return kErrInvalid;
    if (value && std::cmp_greater_equal(target->repository.all(plugin.id, target->scope).size(), kWasmMaxKeys) &&
        !target->repository.get(plugin.id, target->scope, target->key))
        return kErrInvalid;
    target->repository.set(plugin.id, target->scope, target->key, value);
    return kOk;
}

void dataSetI64(void* user, Call& call) { call.ret(store(self(user), call, "n" + std::to_string(call.i64(3)))); }

void dataSetBool(void* user, Call& call) { call.ret(store(self(user), call, call.i32(3) != 0 ? "b1" : "b0")); }

void dataSetStr(void* user, Call& call) {
    const auto value = text(call, 3);
    call.ret(value ? store(self(user), call, "s" + std::string{*value}) : kErrInvalid);
}

void dataRemove(void* user, Call& call) { call.ret(store(self(user), call, std::nullopt)); }

void playerByName(void* user, Call& call) {
    const auto name = text(call, 0);
    call.ret(name ? self(user).host->handleOf(*name) : 0);
}

void playersOnline(void* user, Call& call) {
    WasmHost& host = *self(user).host;
    const auto count = static_cast<std::int32_t>(host.roster().size());
    if (call.i32(1) < count || count == 0)
        return call.ret(count);
    const auto view =
        call.memory().range(static_cast<std::uint32_t>(call.i32(0)), static_cast<std::uint32_t>(count) * 4);
    if (!view)
        return call.ret(kErrInvalid);
    std::int32_t at = 0;
    for (const auto& [name, player] : host.roster()) {
        const std::int32_t handle = host.handleOf(name);
        std::memcpy(view->data() + at * 4, &handle, 4);
        ++at;
    }
    call.ret(count);
}

void playerName(void* user, Call& call) {
    const WasmHost::Player* player = self(user).host->playerOf(call.i32(0));
    call.ret(player != nullptr ? call.memory().put(call.i32(1), call.i32(2), player->name) : kErrStale);
}

void playerSend(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const WasmHost::Player* player = plugin.host->playerOf(call.i32(0));
    if (player == nullptr)
        return call.ret(kErrStale);
    const auto line = resolve(plugin, call, 1, 3);
    if (!line)
        return call.ret(kErrInvalid);
    if (plugin.host->hooks().tell)
        plugin.host->hooks().tell(player->name, *line);
    call.ret(kOk);
}

void playerActionBar(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const WasmHost::Player* player = plugin.host->playerOf(call.i32(0));
    if (player == nullptr)
        return call.ret(kErrStale);
    const auto line = resolve(plugin, call, 1, 3);
    if (!line)
        return call.ret(kErrInvalid);
    runtime::HudTitle title;
    title.actionBar = wire::text::miniMessage::parse(*line);
    call.ret(plugin.host->hooks().hud && plugin.host->hooks().hud(player->name, std::move(title)) ? kOk : kErrInvalid);
}

constexpr std::array kShared = std::to_array<Entry>({
    {kBasaltModule, "log", log},
    {kBasaltModule, "config_i64", configI64},
    {kBasaltModule, "config_f64", configF64},
    {kBasaltModule, "config_bool", configBool},
    {kBasaltModule, "config_str", configStr},
    {kBasaltModule, "translate", translate},
    {kBasaltModule, "data_get_i64", dataGetI64},
    {kBasaltModule, "data_set_i64", dataSetI64},
    {kBasaltModule, "data_get_bool", dataGetBool},
    {kBasaltModule, "data_set_bool", dataSetBool},
    {kBasaltModule, "data_get_str", dataGetStr},
    {kBasaltModule, "data_set_str", dataSetStr},
    {kBasaltModule, "data_remove", dataRemove},
    {kBasaltModule, "player_by_name", playerByName},
    {kBasaltModule, "players_online", playersOnline},
    {kBasaltModule, "player_name", playerName},
    {kBasaltModule, "player_send", playerSend},
    {kBasaltModule, "player_action_bar", playerActionBar},
});

} // namespace

std::string applyArguments(std::string text, std::string_view packed) {
    if (packed.empty() || text.find('{') == std::string::npos)
        return text;
    const auto find = [&packed](std::string_view key) -> std::optional<std::string_view> {
        std::string_view rest = packed;
        const auto word = [&rest]() -> std::optional<std::string_view> {
            std::uint32_t length = 0;
            if (rest.size() < 4)
                return std::nullopt;
            std::memcpy(&length, rest.data(), 4);
            rest.remove_prefix(4);
            if (length > rest.size())
                return std::nullopt;
            const std::string_view value = rest.substr(0, length);
            rest.remove_prefix(length);
            return value;
        };
        if (rest.size() < 4)
            return std::nullopt;
        std::uint32_t count = 0;
        std::memcpy(&count, rest.data(), 4);
        rest.remove_prefix(4);
        for (std::uint32_t entry = 0; entry < std::min<std::uint32_t>(count, 64); ++entry) {
            const auto name = word();
            const auto value = word();
            if (!name || !value)
                return std::nullopt;
            if (*name == key)
                return value;
        }
        return std::nullopt;
    };
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        const std::size_t close = text[index] == '{' ? text.find('}', index) : std::string::npos;
        if (close == std::string::npos) {
            out += text[index++];
            continue;
        }
        const std::string_view name = std::string_view{text}.substr(index + 1, close - index - 1);
        const auto value = find(name);
        if (value)
            out += *value;
        else
            out += std::string_view{text}.substr(index, close - index + 1);
        index = close + 1;
    }
    return out;
}

std::optional<std::string> resolve(const WasmPlugin& plugin, Call& call, std::size_t textIndex, std::size_t argsIndex) {
    const auto message = text(call, textIndex);
    const auto args = call.memory().string(call.i32(argsIndex), call.i32(argsIndex + 1));
    if (!message || !args)
        return std::nullopt;
    std::string line;
    if (message->starts_with('@'))
        line = plugin.text->translation(message->substr(1)).value_or(std::string{message->substr(1)});
    else
        line = std::string{*message};
    if (line.size() > kWasmMaxText)
        line.resize(kWasmMaxText);
    return applyArguments(std::move(line), *args);
}

std::span<const Entry> sharedImports() { return kShared; }

} // namespace wasmimport

std::vector<wire::wasm::HostImport> WasmImports::table(WasmPlugin& plugin) {
    std::vector<wasmimport::Entry> entries{wasmimport::sharedImports().begin(), wasmimport::sharedImports().end()};
    wasmimport::addAllImports(entries);
    std::vector<wire::wasm::HostImport> imports;
    imports.reserve(entries.size());
    for (const wasmimport::Entry& entry : entries)
        imports.push_back({entry.name, entry.fn, &plugin, entry.module});
    return imports;
}

} // namespace basalt::proxy::plugins
