#include <basalt/proxy/config/ConfigDefaults.hpp>
#include <basalt/proxy/config/ProxyConfig.hpp>

#include <algorithm>
#include <array>
#include <deque>

namespace basalt::proxy::config {

wire::config::ConfigDirectory configDirectory(std::string_view directory) {
    return wire::config::ConfigDirectory{
        std::string{directory}, wire::common::Logger{"Config"},
        wire::config::UpgradeRules{.version = "26.3",
                                   .files = {{"proxy.toml",
                                              {"network", "runtime", "admission", "forwarding", "logging", "bedrock",
                                               "reload", "translation", "access", "general", "query", "plugins"}}}}};
}

std::expected<wire::config::GroupsConfig, std::string> loadGroups(std::string_view directory) {
    const auto files = configDirectory(directory);
    const auto root = files.load(wire::config::GroupsConfig::kFileName, kGroupsDefaultToml);
    if (!root)
        return std::unexpected(root.error());
    return wire::config::GroupsConfig::parse(*root, files.pathOf(wire::config::GroupsConfig::kFileName));
}

std::expected<wire::config::CommandsConfig, std::string> loadCommands(std::string_view directory) {
    const auto files = configDirectory(directory);
    const auto root = files.load(wire::config::CommandsConfig::kFileName, kCommandsDefaultToml);
    if (!root)
        return std::unexpected(root.error());
    return wire::config::CommandsConfig::parse(*root, files.pathOf(wire::config::CommandsConfig::kFileName));
}

std::string MessageSet::text(std::string_view section, std::string_view key) const {
    if (const auto* custom = custom_.find(section, key))
        return *custom;
    if (const auto* shipped = defaults_.find(section, key))
        return *shipped;
    return {};
}

std::expected<MessageSet, std::string> loadMessages(std::string_view directory) {
    const auto files = configDirectory(directory);
    const std::string path = files.pathOf(wire::config::MessageCatalog::kFileName);
    const auto shipped = wire::common::toml::parse(kMessagesDefaultToml);
    if (!shipped)
        return std::unexpected("the built-in messages.toml does not parse");
    std::deque<std::string> names;
    std::vector<std::vector<std::string_view>> keys;
    std::vector<wire::config::MessageSection> sections;
    for (const std::string_view section : {"kick", "command"}) {
        keys.emplace_back();
        if (const auto* table = shipped->table(section))
            for (const auto& key : table->keys())
                keys.back().push_back(names.emplace_back(key));
        sections.push_back({section, keys.back()});
    }
    auto defaults = wire::config::MessageCatalog::parse(*shipped, "built-in messages.toml", sections);
    if (!defaults)
        return std::unexpected(defaults.error());
    const auto root = files.load(wire::config::MessageCatalog::kFileName, kMessagesDefaultToml);
    if (!root)
        return std::unexpected(root.error());
    auto custom = wire::config::MessageCatalog::parse(*root, path, sections);
    if (!custom)
        return std::unexpected(custom.error());
    return MessageSet{std::move(*custom), std::move(*defaults)};
}

std::expected<wire::config::ServerListConfig, std::string> loadServerList(std::string_view directory) {
    const auto files = configDirectory(directory);
    auto loaded = wire::config::ServerListConfig::load(files, kServerListDefaultToml);
    if (!loaded)
        return loaded;
    const std::string path = files.pathOf(wire::config::ServerListConfig::kFileName);
    for (const auto& profile : loaded->profiles()) {
        const std::string where = path + ": profiles." + profile.header.name;
        if (profile.source && *profile.source != "profile" && *profile.source != "route" &&
            !(profile.source->starts_with("server:") && profile.source->size() > 7))
            return std::unexpected(where + ".source must be \"profile\", \"route\" or \"server:<name>\"");
        for (const auto& field : profile.relay.value_or(std::vector<std::string>{})) {
            static constexpr std::array<std::string_view, 6> kFields{"online", "max",     "sample",
                                                                     "motd",   "version", "icon"};
            if (std::ranges::find(kFields, field) == kFields.end())
                return std::unexpected(where + ".relay has an unknown field \"" + field + "\"");
        }
    }
    return loaded;
}

std::expected<UiConfig, std::string> loadUi(std::string_view directory) {
    using wire::config::UiPriority;
    const auto files = configDirectory(directory);
    UiConfig ui;
    auto animations = wire::config::AnimationsConfig::load(files, kAnimationsDefaultToml);
    if (!animations)
        return std::unexpected(animations.error());
    auto tabList = wire::config::TabListConfig::load(files, kTabListDefaultToml, UiPriority::kProxy);
    if (!tabList)
        return std::unexpected(tabList.error());
    auto bossBar = wire::config::BossBarConfig::load(files, kBossBarDefaultToml, UiPriority::kBackend);
    if (!bossBar)
        return std::unexpected(bossBar.error());
    auto scoreboard = wire::config::ScoreboardConfig::load(files, kScoreboardDefaultToml, UiPriority::kBackend);
    if (!scoreboard)
        return std::unexpected(scoreboard.error());
    ui.animations = std::move(*animations);
    ui.tabList = std::move(*tabList);
    ui.bossBar = std::move(*bossBar);
    ui.scoreboard = std::move(*scoreboard);
    return ui;
}

} // namespace basalt::proxy::config
