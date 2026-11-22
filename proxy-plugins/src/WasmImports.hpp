#pragma once

#include "WasmPlugin.hpp"

#include <basalt/wire/wasm/Abi.hpp>

#include <span>
#include <vector>

namespace basalt::proxy::plugins::wasmimport {

using wire::wasm::Call;

struct Entry {
    std::string_view module;
    std::string_view name;
    wire::wasm::HostFn fn;
};

[[nodiscard]] std::span<const Entry> sharedImports();
/// Appends the imports of every area: one src/WasmImports<Area>.cpp defining add<Area>Imports (generated).
void addAllImports(std::vector<Entry>& out);

[[nodiscard]] inline WasmPlugin& self(void* user) { return *static_cast<WasmPlugin*>(user); }

[[nodiscard]] inline std::optional<std::string_view> text(Call& call, std::size_t index) {
    return call.memory().string(call.i32(index), call.i32(index + 1));
}

/// The text of a message argument: a '@' key looked up in the plugin's language file, then its {name} arguments.
[[nodiscard]] std::optional<std::string> resolve(const WasmPlugin& plugin, Call& call, std::size_t textIndex,
                                                 std::size_t argsIndex);

[[nodiscard]] std::string applyArguments(std::string text, std::string_view packed);

} // namespace basalt::proxy::plugins::wasmimport
