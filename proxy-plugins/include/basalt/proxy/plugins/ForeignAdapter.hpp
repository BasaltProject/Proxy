#pragma once

#include <basalt/proxy/runtime/ProxyRuntime.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::plugins {

struct ForeignPlugin {
    std::string name;
    std::string version;
};

/// A plugin system from another proxy running on its own runtime. Every call arrives on the plugin thread.
class ForeignAdapter {
public:
    virtual ~ForeignAdapter() = default;

    /// The tag `/plugins` shows before each plugin name.
    [[nodiscard]] virtual std::string_view label() const = 0;
    [[nodiscard]] virtual std::vector<ForeignPlugin> plugins() const = 0;
    /// Roots of the commands the foreign plugins registered.
    [[nodiscard]] virtual std::vector<std::string> commands() const = 0;
    [[nodiscard]] virtual bool decides(runtime::DecisionKind kind) const = 0;
    [[nodiscard]] virtual std::optional<runtime::DecisionResult> decide(const runtime::DecisionRequest& request) = 0;
    virtual void notify(const runtime::PlayerEvent& event) = 0;
    [[nodiscard]] virtual bool listens(std::string_view channel) const = 0;

    [[nodiscard]] virtual std::vector<std::string> channels() const { return {}; }

    /// True when a foreign handler cancelled the message.
    [[nodiscard]] virtual bool message(std::string_view player, std::string_view channel,
                                       std::span<const std::uint8_t> data, bool fromBackend) = 0;
    [[nodiscard]] virtual bool console(std::string_view line) = 0;
};

} // namespace basalt::proxy::plugins
