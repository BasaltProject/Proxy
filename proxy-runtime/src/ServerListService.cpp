#include "ServerListService.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/config/BasicPlaceholders.hpp>
#include <basalt/wire/text/Serializers.hpp>

#include <algorithm>

namespace basalt::proxy::runtime::detail {

ServerListService::ServerListService(std::string directory, wire::config::ServerListConfig initial, Sources sources)
    : directory_(std::move(directory)), sources_(std::move(sources)), config_(std::move(initial)) {
    wire::config::registerBasicPlaceholders(placeholders_, {.versionName = std::string{buildInfo::kMinecraftVersion},
                                                            .protocolVersion = buildInfo::kProtocolVersion,
                                                            .online = sources_.online,
                                                            .maxPlayers = [slots = sources_.slots] { return slots; }});
    wire::config::ServerListStatus::Hooks hooks;
    hooks.playerByAddress = [this](const std::string& address) {
        return sources_.players != nullptr ? sources_.players->nameByAddress(address) : std::nullopt;
    };
    status_ = std::make_unique<wire::config::ServerListStatus>(
        [this] { return config_.current(); }, placeholders_, [] { return std::vector<wire::config::ListedPlayer>{}; },
        wire::config::ServerListStatus::Options{.slots = sources_.slots,
                                                .versionName = std::string{buildInfo::kMinecraftVersion},
                                                .protocolVersion = buildInfo::kProtocolVersion,
                                                .configRoot = directory_},
        std::move(hooks), wire::common::Logger{"ServerList"});
}

std::expected<std::unique_ptr<ServerListService>, std::string>
ServerListService::create(std::string_view configDirectory, Sources sources) {
    auto loaded = config::loadServerList(configDirectory);
    if (!loaded)
        return std::unexpected(loaded.error());
    return std::unique_ptr<ServerListService>{
        new ServerListService{std::string{configDirectory}, std::move(*loaded), std::move(sources)}};
}

wire::protocol::status::ServerStatusPayload ServerListService::statusFor(std::int32_t protocol, std::string_view host,
                                                                         std::string_view address) {
    return status_->statusFor(protocol, std::string{host}, std::string{address});
}

std::string ServerListService::bedrockMotd() {
    const auto payload = status_->statusFor(buildInfo::kProtocolVersion, "", std::nullopt);
    std::string line = wire::text::toPlainText(payload.description);
    line = line.substr(0, line.find('\n'));
    std::ranges::replace(line, ';', ',');
    const auto first = line.find_first_not_of(' ');
    line = first == std::string::npos ? std::string{} : line.substr(first);
    while (!line.empty() && line.back() == ' ')
        line.pop_back();
    return line.empty() ? "Basalt Proxy" : line.substr(0, 64);
}

bool ServerListService::rememberIp() const { return config_.current()->rememberIp(); }

std::expected<void, std::string> ServerListService::reload() {
    auto fresh = config::loadServerList(directory_);
    if (!fresh)
        return std::unexpected(fresh.error());
    config_.replace(std::move(*fresh));
    return {};
}

} // namespace basalt::proxy::runtime::detail
