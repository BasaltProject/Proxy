#include <basalt/proxy/config/ConfigDefaults.hpp>
#include <basalt/proxy/config/ProxyConfig.hpp>

namespace basalt::proxy::config {

wire::config::ConfigDirectory configDirectory(std::string_view directory) {
    return wire::config::ConfigDirectory{
        std::string{directory}, wire::common::Logger{"Config"},
        wire::config::UpgradeRules{.version = "26.3",
                                   .files = {{"proxy.toml",
                                              {"network", "runtime", "admission", "forwarding", "logging", "bedrock",
                                               "reload", "translation"}}}}};
}

std::expected<wire::config::ServerListConfig, std::string> loadServerList(std::string_view directory) {
    return wire::config::ServerListConfig::load(configDirectory(directory), kServerListDefaultToml);
}

std::expected<UiConfig, std::string> loadUi(std::string_view directory) {
    using wire::config::UiPriority;
    const auto files = configDirectory(directory);
    UiConfig ui;
    auto animations = wire::config::AnimationsConfig::load(files, kAnimationsDefaultToml);
    if (!animations)
        return std::unexpected(animations.error());
    auto tabList = wire::config::TabListConfig::load(files, kTabListDefaultToml, UiPriority::Proxy);
    if (!tabList)
        return std::unexpected(tabList.error());
    auto bossBar = wire::config::BossBarConfig::load(files, kBossBarDefaultToml, UiPriority::Backend);
    if (!bossBar)
        return std::unexpected(bossBar.error());
    auto scoreboard = wire::config::ScoreboardConfig::load(files, kScoreboardDefaultToml, UiPriority::Backend);
    if (!scoreboard)
        return std::unexpected(scoreboard.error());
    ui.animations = std::move(*animations);
    ui.tabList = std::move(*tabList);
    ui.bossBar = std::move(*bossBar);
    ui.scoreboard = std::move(*scoreboard);
    return ui;
}

} // namespace basalt::proxy::config
