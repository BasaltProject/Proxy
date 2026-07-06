#include "ServerListService.hpp"

#include "DefaultIcon.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/config/BasicPlaceholders.hpp>
#include <basalt/wire/text/Serializers.hpp>

#include <algorithm>

namespace basalt::proxy::runtime::detail {
namespace {
/// What the ping being answered on this thread takes from a relayed status, so the placeholders agree with the list.
struct RelayedNumbers {
    std::optional<std::int32_t> online;
    std::optional<std::int32_t> max;
};

thread_local RelayedNumbers tRelayed;
} // namespace

ServerListService::ServerListService(std::string directory, wire::config::ServerListConfig initial, Sources sources)
    : directory_(std::move(directory)), sources_(std::move(sources)), config_(std::move(initial)) {
    wire::config::registerBasicPlaceholders(
        placeholders_, {.versionName = std::string{buildInfo::kMinecraftVersion},
                        .protocolVersion = buildInfo::kProtocolVersion,
                        .online =
                            [this] {
                                if (tRelayed.online)
                                    return *tRelayed.online;
                                const std::lock_guard lock{rosterMutex_};
                                return static_cast<std::int32_t>(roster_.size());
                            },
                        .maxPlayers = [slots = sources_.slots] { return tRelayed.max.value_or(slots); }});
    wire::config::ServerListStatus::Hooks hooks;
    hooks.playerByAddress = [this](const std::string& address) {
        return sources_.players != nullptr ? sources_.players->nameByAddress(address) : std::nullopt;
    };
    hooks.relay = [this](const std::string& source, const std::string& host, const std::vector<std::string>& fields) {
        auto answer = sources_.relay ? sources_.relay(source, host) : std::nullopt;
        if (answer && answer->players) {
            const auto takes = [&fields](std::string_view name) {
                return std::ranges::find(fields, name) != fields.end();
            };
            if (takes("online"))
                tRelayed.online = answer->players->online;
            if (takes("max"))
                tRelayed.max = answer->players->max;
        }
        return answer;
    };
    status_ = std::make_unique<wire::config::ServerListStatus>(
        [this] { return config_.current(); }, placeholders_,
        [this] {
            std::vector<wire::config::ListedPlayer> listed;
            const std::lock_guard lock{rosterMutex_};
            for (const auto& [uuid, name] : roster_)
                listed.push_back({.name = name, .uuid = uuid});
            return listed;
        },
        wire::config::ServerListStatus::Options{.slots = sources_.slots,
                                                .versionName = std::string{buildInfo::kMinecraftVersion},
                                                .protocolVersion = buildInfo::kProtocolVersion,
                                                .configRoot = directory_,
                                                .builtInIcon = defaultIcon()},
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
    tRelayed = {};
    auto payload = status_->statusFor(protocol, std::string{host}, std::string{address});
    tRelayed = {};
    return payload;
}

std::string ServerListService::bedrockMotd() {
    tRelayed = {};
    const auto payload = status_->statusFor(buildInfo::kProtocolVersion, "", std::nullopt);
    tRelayed = {};
    std::string line = wire::text::toPlainText(payload.description);
    line = line.substr(0, line.find('\n'));
    std::ranges::replace(line, ';', ',');
    const auto first = line.find_first_not_of(' ');
    line = first == std::string::npos ? std::string{} : line.substr(first);
    while (!line.empty() && line.back() == ' ')
        line.pop_back();
    return line.empty() ? "Basalt Proxy" : line.substr(0, 64);
}

void ServerListService::playerJoined(std::string_view uuid, std::string_view name) {
    const std::lock_guard lock{rosterMutex_};
    roster_[std::string{uuid}] = std::string{name};
}

void ServerListService::playerLeft(std::string_view uuid) {
    const std::lock_guard lock{rosterMutex_};
    if (const auto found = roster_.find(uuid); found != roster_.end())
        roster_.erase(found);
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
