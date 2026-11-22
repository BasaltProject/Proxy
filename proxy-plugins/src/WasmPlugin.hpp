#pragma once

#include "WasmHost.hpp"

#include <basalt/wire/plugins/PluginManifest.hpp>

namespace basalt::proxy::plugins {

constexpr std::chrono::milliseconds kWasmCallBudget{50};
constexpr std::chrono::milliseconds kWasmInitBudget{2000};
constexpr std::size_t kWasmMaxText = 4096;
constexpr std::int32_t kWasmMaxKeys = 1000;
constexpr int kWasmStrikeLimit = 5;

struct WasmHandler {
    std::int32_t priority = 2;
    std::int32_t token = 0;
};

struct WasmPlugin {
    std::string id;
    std::string name;
    std::string version;
    WasmHost* host = nullptr;
    wire::common::toml::TomlTable config;
    std::unique_ptr<wire::plugins::PluginText> text;
    wire::plugins::PluginCapabilities capabilities;
    std::vector<std::string> authors;
    std::vector<wire::plugins::PluginDependency> dependencies;

    [[nodiscard]] bool capable(std::string_view name) const { return capabilities.has(name); }

    std::optional<wire::wasm::Module> module;
    std::optional<wire::wasm::Instance> instance;
    std::vector<wire::wasm::HostImport> imports;
    std::array<std::vector<WasmHandler>, kWasmEventCount> handlers;
    std::array<std::vector<WasmHandler>, kWasmEventCount> watchers;
    int strikes = 0;
    bool broken = false;
    bool enabled = false;
    std::map<std::string, std::shared_ptr<void>, std::less<>> areas;
};

/// The host functions of the proxy profile, bound to one plugin.
struct WasmImports {
    [[nodiscard]] static std::vector<wire::wasm::HostImport> table(WasmPlugin& plugin);
};

} // namespace basalt::proxy::plugins
