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

} // namespace basalt::proxy::config
