#pragma once

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/common/placeholder/Placeholders.hpp>
#include <basalt/wire/config/LiveValue.hpp>

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace basalt::proxy::runtime::detail {

/// The tab list, boss bar, scoreboard and animations files, held live; reloaded together or not at all.
class UiService {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<UiService>, std::string>
    create(std::string_view configDirectory, wire::common::placeholder::PlaceholderRegistry& placeholders);

    [[nodiscard]] std::shared_ptr<const config::UiConfig> current() const { return config_.current(); }

    [[nodiscard]] std::expected<void, std::string> reload();

private:
    UiService(std::string directory, config::UiConfig initial)
        : directory_(std::move(directory)), config_(std::move(initial)) {}

    std::string directory_;
    wire::config::LiveValue<config::UiConfig> config_;
};

} // namespace basalt::proxy::runtime::detail
