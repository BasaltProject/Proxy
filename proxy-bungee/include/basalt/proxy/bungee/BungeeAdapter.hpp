#pragma once

#include <basalt/proxy/plugins/JavaAdapter.hpp>

namespace basalt::proxy::bungee {

[[nodiscard]] inline std::unique_ptr<plugins::ForeignAdapter>
createBungeeAdapter(const plugins::JavaRuntimeOptions& options, const plugins::PluginHooks& hooks) {
    return plugins::createJavaAdapter(options, hooks,
                                      {.label = "Bungee",
                                       .logName = "bungee",
                                       .facade = "org/basaltmc/bungee/BasaltBungee",
                                       .natives = "org/basaltmc/bungee/Natives",
                                       .kind = plugins::ForeignKind::kBungee,
                                       .slot = 0});
}

} // namespace basalt::proxy::bungee
