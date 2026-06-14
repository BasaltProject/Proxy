#pragma once

#include <basalt/proxy/plugins/JavaAdapter.hpp>

namespace basalt::proxy::velocity {

[[nodiscard]] inline std::unique_ptr<plugins::ForeignAdapter>
createVelocityAdapter(const plugins::JavaRuntimeOptions& options, const plugins::PluginHooks& hooks) {
    return plugins::createJavaAdapter(options, hooks,
                                      {.label = "Velocity",
                                       .logName = "velocity",
                                       .facade = "org/basaltmc/velocity/BasaltVelocity",
                                       .natives = "org/basaltmc/velocity/Natives",
                                       .kind = plugins::ForeignKind::kVelocity,
                                       .slot = 1});
}

} // namespace basalt::proxy::velocity
