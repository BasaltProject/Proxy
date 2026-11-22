#pragma once

#include "WasmHost.hpp"

#include <basalt/wire/wasm/Value.hpp>

#include <span>

namespace basalt::proxy::plugins {

enum class EventFieldType : std::uint8_t { kString, kInt, kBool, kList };

struct EventField {
    std::string_view name;
    EventFieldType type;
    bool readable;
    bool writable;
};

/// The fields the event of `call` has, with what a plugin may do to each; the one table get, set and fields share.
[[nodiscard]] std::span<const EventField> eventFields(const WasmCall& call);

[[nodiscard]] std::string_view eventFieldTypeName(EventFieldType type);

/// The current value of a field, or nothing when the event does not have it or it cannot be read.
[[nodiscard]] std::optional<wire::wasm::Value> readEventField(const WasmCall& call, std::string_view name);

/// Sets a field of a decided event: kOk, kErrAbsent, or kErrInvalid (wrong type, read only, limits).
[[nodiscard]] std::int32_t writeEventField(WasmPlugin& plugin, WasmCall& call, std::string_view name,
                                           const wire::wasm::Value& value);

} // namespace basalt::proxy::plugins
