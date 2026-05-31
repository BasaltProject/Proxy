#pragma once

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/common/placeholder/Placeholders.hpp>
#include <basalt/wire/config/BasicPlaceholders.hpp>
#include <basalt/wire/config/LiveValue.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace basalt::proxy::runtime::detail {

using MessageArgs = std::vector<std::pair<std::string_view, std::string>>;

/// The texts of messages.toml: MiniMessage with %name% arguments and the shared %basalt_*% placeholders.
class Messages {
public:
    Messages(config::MessageSet initial, wire::config::BasicFacts facts);

    void reload(config::MessageSet next) { texts_.replace(std::move(next)); }

    /// MiniMessage, ready to send; argument values are escaped so a player name cannot inject markup.
    [[nodiscard]] std::string kick(std::string_view key, const MessageArgs& args = {}) const {
        return render("kick", key, args);
    }

    [[nodiscard]] std::string command(std::string_view key, const MessageArgs& args = {}) const {
        return render("command", key, args);
    }

    /// Fills in the placeholders of a text written elsewhere, a custom command's for instance.
    [[nodiscard]] std::string fill(std::string_view text) const;

    [[nodiscard]] static std::string escape(std::string_view text);

    [[nodiscard]] static std::string plain(std::string_view miniMessage);

private:
    [[nodiscard]] std::string render(std::string_view section, std::string_view key, const MessageArgs& args) const;

    wire::config::LiveValue<config::MessageSet> texts_;
    wire::common::placeholder::PlaceholderRegistry placeholders_;
};

} // namespace basalt::proxy::runtime::detail
