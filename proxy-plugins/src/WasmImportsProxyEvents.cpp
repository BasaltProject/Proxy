#include "WasmEventFields.hpp"
#include "WasmImports.hpp"
#include "WasmValues.hpp"

#include <algorithm>

namespace basalt::proxy::plugins::wasmimport {
namespace {

using namespace wire::wasm;

/// The live event context of a call, or the error to answer: STALE when dead, INVALID when it is not an event.
[[nodiscard]] WasmCall* eventOf(void* user, Call& call, std::int32_t& error) {
    WasmCall* live = self(user).host->live(call.i32(0));
    error = kOk;
    if (live == nullptr)
        error = kErrStale;
    else if (live->command || live->value != nullptr)
        error = kErrInvalid;
    return error == kOk ? live : nullptr;
}

void eventGet(void* user, Call& call) {
    std::int32_t error = kOk;
    const WasmCall* live = eventOf(user, call, error);
    const auto field = text(call, 1);
    if (live == nullptr)
        return call.ret(error);
    if (!field)
        return call.ret(kErrInvalid);
    if (field->empty()) {
        Value::Map all;
        for (const EventField& known : eventFields(*live))
            if (auto value = readEventField(*live, known.name))
                all.push_back({std::string{known.name}, std::move(*value)});
        return call.ret(putValue(call.memory(), call.i32(3), call.i32(4), Value{std::move(all)}));
    }
    const auto value = readEventField(*live, *field);
    if (!value)
        return call.ret(kErrAbsent);
    call.ret(putValue(call.memory(), call.i32(3), call.i32(4), *value));
}

void eventSet(void* user, Call& call) {
    std::int32_t error = kOk;
    WasmCall* live = eventOf(user, call, error);
    const auto field = text(call, 1);
    if (live == nullptr)
        return call.ret(error);
    const auto value = readValue(call.memory(), call.i32(3), call.i32(4));
    if (!field || !value)
        return call.ret(kErrInvalid);
    call.ret(writeEventField(self(user), *live, *field, *value));
}

void eventFieldsList(void* user, Call& call) {
    std::int32_t error = kOk;
    const WasmCall* live = eventOf(user, call, error);
    if (live == nullptr)
        return call.ret(error);
    Value::List list;
    for (const EventField& field : eventFields(*live)) {
        Value::Map entry;
        entry.push_back({"name", Value{std::string{field.name}}});
        entry.push_back({"type", Value{std::string{eventFieldTypeName(field.type)}}});
        entry.push_back({"readable", Value{field.readable}});
        entry.push_back({"writable", Value{field.writable && live->decision != nullptr}});
        list.emplace_back(std::move(entry));
    }
    call.ret(putValue(call.memory(), call.i32(1), call.i32(2), Value{std::move(list)}));
}

void eventWatch(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const auto name = text(call, 0);
    const auto event = name ? wasmEventOf(*name) : std::nullopt;
    if (!event)
        return call.ret(kErrInvalid);
    if (*event == WasmEvent::kChat || *event == WasmEvent::kKick) {
        plugin.watchers[static_cast<std::size_t>(*event)].push_back(WasmHandler{2, call.i32(2)});
        return call.ret(kOk);
    }
    if (*event != WasmEvent::kJoin && *event != WasmEvent::kQuit && *event != WasmEvent::kServerChange)
        return call.ret(kErrInvalid);
    plugin.handlers[static_cast<std::size_t>(*event)].push_back(WasmHandler{2, call.i32(2)});
    call.ret(kOk);
}

} // namespace

void addProxyEventsImports(std::vector<Entry>& out) {
    out.push_back({kProxyModule, "event_get", eventGet});
    out.push_back({kProxyModule, "event_set", eventSet});
    out.push_back({kProxyModule, "event_fields", eventFieldsList});
    out.push_back({kProxyModule, "event_watch", eventWatch});
}

} // namespace basalt::proxy::plugins::wasmimport
