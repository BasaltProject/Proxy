#pragma once

#include "BedrockStatus.hpp"
#include "CommandService.hpp"
#include "PermissionService.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/plugins/PluginHost.hpp>
#include <basalt/proxy/runtime/CommandDispatcher.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/plugins/PacketRegistry.hpp>

#include <atomic>
#include <expected>
#include <memory>
#include <string>

namespace basalt::proxy::runtime::detail {

struct PluginSetup {
    plugins::PluginSettings settings;
    plugins::PluginHooks hooks;
};

PluginSetup makePluginSetup(const std::expected<config::ProxyConfig, std::string>& configuration,
                            std::expected<ProxyRuntime, std::string>& runtime,
                            std::atomic<BedrockStatus*>& bedrockPlayers,
                            const decltype(plugins::PluginHooks::tell)& tellPlayer,
                            const decltype(plugins::PluginHooks::kick)& kickPlayer, CommandService& commandService,
                            const CommandSource& consoleSource, CommandDispatcher& dispatcher,
                            PermissionService& permissions,
                            const std::shared_ptr<wire::plugins::PacketRegistry>& packetRegistry);

} // namespace basalt::proxy::runtime::detail
