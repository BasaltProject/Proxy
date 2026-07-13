#pragma once

#include <basalt/proxy/plugins/JarDetector.hpp>
#include <basalt/proxy/plugins/PluginHost.hpp>

#include <memory>
#include <string>

namespace basalt::proxy::plugins {

struct JavaRuntimeOptions {
    /// Where the jars to load are; no virtual machine starts when none of them belongs to a foreign proxy.
    std::string pluginsDirectory = "plugins";
    /// Holds shim/, bungee/ and velocity/ (a facade jar and a lib/ folder each); BASALT_SHIM_DIR overrides it.
    std::string runtimeDirectory = ".basalt";
};

/// Which facade to run; the machine itself is shared by every adapter in the process.
struct JavaSpec {
    std::string label;
    std::string logName;
    std::string facade;
    std::string natives;
    ForeignKind kind = ForeignKind::kBungee;
    int slot = 0;
};

/// Null when there is nothing to run or the machine cannot start; the reason goes to the log hook.
[[nodiscard]] std::unique_ptr<ForeignAdapter> createJavaAdapter(const JavaRuntimeOptions& options,
                                                                const PluginHooks& hooks, JavaSpec spec);

} // namespace basalt::proxy::plugins
