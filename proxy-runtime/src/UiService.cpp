#include "UiService.hpp"

#include <basalt/wire/config/AnimationPlaceholders.hpp>

#include <chrono>

namespace basalt::proxy::runtime::detail {

std::expected<std::unique_ptr<UiService>, std::string>
UiService::create(std::string_view configDirectory, wire::common::placeholder::PlaceholderRegistry& placeholders) {
    auto loaded = config::loadUi(configDirectory);
    if (!loaded)
        return std::unexpected(loaded.error());
    std::unique_ptr<UiService> service{new UiService{std::string{configDirectory}, std::move(*loaded)}};
    placeholders.registerProvider(
        std::string{wire::config::kAnimationIdentifier},
        wire::config::animationPlaceholders(wire::config::AnimationPlaceholderSources{
            .animations =
                [live = service.get()] {
                    const auto snapshot = live->current();
                    return std::shared_ptr<const wire::config::AnimationsConfig>{snapshot, &snapshot->animations};
                },
            .registry = [&placeholders]() -> const wire::common::placeholder::PlaceholderRegistry& {
                return placeholders;
            },
            .nowMillis =
                [] {
                    return std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                        .count();
                }}));
    return service;
}

std::expected<void, std::string> UiService::reload() {
    auto fresh = config::loadUi(directory_);
    if (!fresh)
        return std::unexpected(fresh.error());
    config_.replace(std::move(*fresh));
    return {};
}

} // namespace basalt::proxy::runtime::detail
