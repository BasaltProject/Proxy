#pragma once

#include "WasmHandles.hpp"
#include "WasmPlugin.hpp"

#include <basalt/wire/wasm/Memory.hpp>
#include <basalt/wire/wasm/Value.hpp>

#include <expected>

namespace basalt::proxy::plugins {

/// The state an area keeps between calls, created on first use (the area name is the key).
template <typename T>
[[nodiscard]] T& areaState(WasmPlugin& plugin, std::string_view area) {
    std::shared_ptr<void>& slot = plugin.areas.try_emplace(std::string{area}).first->second;
    if (!slot) {
        slot = std::make_shared<T>();
    }
    return *static_cast<T*>(slot.get());
}

/// One value out of guest memory; the error is the ABI result (INVALID for a bad range, limit breach or bad bytes).
[[nodiscard]] inline std::expected<wire::wasm::Value, std::int32_t> readValue(const wire::wasm::GuestMemory& memory,
                                                                              std::int32_t ptr, std::int32_t len) {
    if (len < 0 || static_cast<std::size_t>(len) > wire::wasm::kValueMaxBytes) {
        return std::unexpected(wire::wasm::kErrInvalid);
    }
    const auto bytes = memory.range(static_cast<std::uint32_t>(ptr), static_cast<std::uint32_t>(len));
    if (!bytes) {
        return std::unexpected(wire::wasm::kErrInvalid);
    }
    std::expected<wire::wasm::Value, std::string> value = wire::wasm::decodeValue(*bytes);
    if (!value) {
        return std::unexpected(wire::wasm::kErrInvalid);
    }
    return std::move(*value);
}

/// Writes the encoding with the needed-length convention; INVALID when the value breaks a limit.
[[nodiscard]] inline std::int32_t putValue(const wire::wasm::GuestMemory& memory, std::int32_t ptr, std::int32_t cap,
                                           const wire::wasm::Value& value) {
    std::string encoded;
    if (!wire::wasm::encodeValue(value, encoded)) {
        return wire::wasm::kErrInvalid;
    }
    return memory.put(ptr, cap, encoded);
}

/// Runs `token` with a completion context whose value_result answers `value`; false when it did not run.
inline bool deliverValue(WasmPlugin& plugin, std::int32_t token, const wire::wasm::Value& value) {
    WasmCall call;
    call.value = &value;
    return plugin.host->dispatch(plugin, token, &call, "completion", kWasmCallBudget);
}

/// Pending completions of a plugin by request handle; each holds the token to call back.
using RequestTable = HandleTable<std::int32_t>;

[[nodiscard]] inline RequestTable& requests(WasmPlugin& plugin) {
    return areaState<RequestTable>(plugin, "core_values");
}

/// The request handle an area returns for a call that completes later; 0 when too many are pending.
[[nodiscard]] inline std::int32_t beginRequest(WasmPlugin& plugin, std::int32_t token) {
    return requests(plugin).add(token);
}

/// Delivers `value` to the request's token; false when it was cancelled or already completed.
inline bool completeRequest(WasmPlugin& plugin, std::int32_t request, const wire::wasm::Value& value) {
    std::int32_t* token = nullptr;
    if (requests(plugin).find(request, token) != wire::wasm::kOk) {
        return false;
    }
    const std::int32_t target = *token;
    static_cast<void>(requests(plugin).remove(request));
    return deliverValue(plugin, target, value);
}

} // namespace basalt::proxy::plugins
