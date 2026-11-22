#include "WasmImports.hpp"
#include "WasmValues.hpp"

#include <algorithm>

namespace basalt::proxy::plugins::wasmimport {
namespace {

using namespace wire::wasm;
using wire::common::toml::TomlTable;
using wire::common::toml::TomlValue;

[[nodiscard]] Value valueOf(const TomlValue& toml) {
    if (const auto* line = toml.get<std::string>())
        return Value{*line};
    if (const auto* whole = toml.get<std::int64_t>())
        return Value{*whole};
    if (const auto* real = toml.get<double>())
        return Value{*real};
    if (const auto* flag = toml.get<bool>())
        return Value{*flag};
    if (const auto* array = toml.get<std::vector<TomlValue>>()) {
        Value::List list;
        for (const TomlValue& item : *array)
            list.push_back(valueOf(item));
        return Value{std::move(list)};
    }
    Value::Map map;
    if (const auto* table = toml.get<TomlTable>())
        for (const std::string& key : table->keys())
            map.push_back({key, valueOf(*table->find(key))});
    return Value{std::move(map)};
}

[[nodiscard]] const TomlValue* findIn(const TomlTable& root, std::string_view key, bool dashed) {
    const TomlTable* table = &root;
    while (true) {
        const auto dot = key.find('.');
        std::string part{key.substr(0, dot)};
        if (dashed)
            std::ranges::replace(part, '_', '-');
        if (dot == std::string_view::npos)
            return table->find(part);
        table = table->table(part);
        if (table == nullptr)
            return nullptr;
        key.remove_prefix(dot + 1);
    }
}

void configValue(void* user, Call& call) {
    const TomlTable& config = self(user).config;
    const auto key = text(call, 0);
    if (!key)
        return call.ret(kErrInvalid);
    if (key->empty())
        return call.ret(putValue(call.memory(), call.i32(2), call.i32(3), valueOf(TomlValue::ofTable(config))));
    const TomlValue* found = findIn(config, *key, false);
    if (found == nullptr)
        found = findIn(config, *key, true);
    if (found == nullptr)
        return call.ret(kErrAbsent);
    call.ret(putValue(call.memory(), call.i32(2), call.i32(3), valueOf(*found)));
}

void pluginInfo(void* user, Call& call) {
    const WasmPlugin& plugin = self(user);
    const auto field = text(call, 0);
    if (!field || (*field != "id" && *field != "name" && *field != "version"))
        return call.ret(kErrInvalid);
    const std::string& value = *field == "id" ? plugin.id : *field == "name" ? plugin.name : plugin.version;
    call.ret(call.memory().put(call.i32(2), call.i32(3), value));
}

void pluginManifest(void* user, Call& call) {
    const WasmPlugin& plugin = self(user);
    const auto strings = [](const auto& names) {
        Value::List list;
        for (const std::string& name : names)
            list.emplace_back(name);
        return Value{std::move(list)};
    };
    Value::List depends;
    for (const auto& dependency : plugin.dependencies) {
        Value::Map entry;
        entry.push_back({"id", Value{dependency.id}});
        entry.push_back({"requirement", Value{dependency.requirement}});
        entry.push_back({"hard", Value{dependency.hard}});
        depends.emplace_back(std::move(entry));
    }
    Value::Map manifest;
    manifest.push_back({"id", Value{plugin.id}});
    manifest.push_back({"name", Value{plugin.name}});
    manifest.push_back({"version", Value{plugin.version}});
    manifest.push_back({"authors", strings(plugin.authors)});
    manifest.push_back({"capabilities", strings(plugin.capabilities.declared)});
    manifest.push_back({"depends", Value{std::move(depends)}});
    call.ret(putValue(call.memory(), call.i32(0), call.i32(1), Value{std::move(manifest)}));
}

[[nodiscard]] const wire::plugins::CommandValue* argumentOf(const WasmCall& live, std::string_view name) {
    for (std::size_t index = 0; index < live.spec->arguments.size() && index < live.values->size(); ++index)
        if (live.spec->arguments[index].name == name)
            return &(*live.values)[index];
    return nullptr;
}

[[nodiscard]] WasmCall* commandOf(void* user, Call& call) {
    WasmCall* live = self(user).host->live(call.i32(0));
    return live != nullptr && live->command ? live : nullptr;
}

void commandArgF64(void* user, Call& call) {
    const WasmCall* live = commandOf(user, call);
    const auto name = text(call, 1);
    if (live == nullptr)
        return call.ret(kErrStale);
    const auto* value = name ? argumentOf(*live, *name) : nullptr;
    if (value == nullptr || std::holds_alternative<std::monostate>(*value))
        return call.ret(kErrAbsent);
    double number = 0;
    if (const auto* real = std::get_if<double>(value))
        number = *real;
    else if (const auto* whole = std::get_if<std::int64_t>(value))
        number = static_cast<double>(*whole);
    else
        return call.ret(kErrInvalid);
    call.ret(call.memory().write(call.i32(3), number) ? kOk : kErrInvalid);
}

void commandArgBool(void* user, Call& call) {
    const WasmCall* live = commandOf(user, call);
    const auto name = text(call, 1);
    if (live == nullptr)
        return call.ret(kErrStale);
    const auto* value = name ? argumentOf(*live, *name) : nullptr;
    if (value == nullptr || std::holds_alternative<std::monostate>(*value))
        return call.ret(kErrAbsent);
    const auto* flag = std::get_if<bool>(value);
    if (flag == nullptr)
        return call.ret(kErrInvalid);
    call.ret(call.memory().write(call.i32(3), static_cast<std::int32_t>(*flag)) ? kOk : kErrInvalid);
}

void commandArgs(void* user, Call& call) {
    const WasmCall* live = commandOf(user, call);
    if (live == nullptr)
        return call.ret(kErrStale);
    Value::Map all;
    for (std::size_t index = 0; index < live->spec->arguments.size() && index < live->values->size(); ++index) {
        const auto& argument = (*live->values)[index];
        Value value;
        if (const auto* flag = std::get_if<bool>(&argument))
            value = Value{*flag};
        else if (const auto* whole = std::get_if<std::int64_t>(&argument))
            value = Value{*whole};
        else if (const auto* real = std::get_if<double>(&argument))
            value = Value{*real};
        else if (const auto* line = std::get_if<std::string>(&argument))
            value = Value{*line};
        all.push_back({live->spec->arguments[index].name, std::move(value)});
    }
    call.ret(putValue(call.memory(), call.i32(1), call.i32(2), Value{std::move(all)}));
}

} // namespace

void addProxyEventsExtraImports(std::vector<Entry>& out) {
    out.push_back({kProxyModule, "plugin_info", pluginInfo});
    out.push_back({kProxyModule, "plugin_manifest", pluginManifest});
    out.push_back({kProxyModule, "config_value", configValue});
    out.push_back({kProxyModule, "command_arg_f64", commandArgF64});
    out.push_back({kProxyModule, "command_arg_bool", commandArgBool});
    out.push_back({kProxyModule, "command_args", commandArgs});
}

} // namespace basalt::proxy::plugins::wasmimport
