#pragma once

#include "WasmHandles.hpp"
#include "WasmPlugin.hpp"
#include "WasmValues.hpp"

#include <basalt/wire/permissions/PermissionChain.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace basalt::proxy::plugins {

inline constexpr std::string_view kAdminArea = "proxy_admin";
inline constexpr std::chrono::milliseconds kProviderDeadline{25};

/// One question put to a permission provider: shared by the asking thread and the plugin thread.
struct PermissionAsk {
    std::mutex mutex;
    std::condition_variable done;
    bool meta = false;
    bool open = true;
    bool finished = false;
    wire::permissions::Tristate answer = wire::permissions::Tristate::kUndefined;
    std::optional<std::string> text;
};

/// A provider a plugin put in the chain; the chain's lambdas hold it weakly so a removed one answers nothing.
struct ProviderSlot {
    WasmPlugin* plugin = nullptr;
    std::int32_t checkToken = 0;
    std::int32_t metaToken = 0;
    wire::permissions::ProviderHandle handle;
};

struct AdminState {
    /// Late work holds this weakly: it is gone once the plugin is.
    std::shared_ptr<WasmPlugin*> life = std::make_shared<WasmPlugin*>(nullptr);
    HandleTable<std::shared_ptr<ProviderSlot>> providers;
    std::shared_ptr<PermissionAsk> ask;
    const wire::wasm::Value* askValue = nullptr;

    ~AdminState() {
        providers.forEach([](std::int32_t, std::shared_ptr<ProviderSlot>& slot) { slot->handle.remove(); });
    }
};

[[nodiscard]] inline AdminState& adminState(WasmPlugin& plugin) {
    AdminState& state = areaState<AdminState>(plugin, kAdminArea);
    *state.life = &plugin;
    return state;
}

} // namespace basalt::proxy::plugins
