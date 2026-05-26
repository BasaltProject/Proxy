#pragma once

#include "PlayerDirectory.hpp"

#include <basalt/wire/common/placeholder/Placeholders.hpp>
#include <basalt/wire/config/LiveValue.hpp>
#include <basalt/wire/config/ServerListConfig.hpp>
#include <basalt/wire/config/ServerListStatus.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace basalt::proxy::runtime::detail {

/// What the multiplayer list shows, from server_list.toml, for Java pings and the Bedrock advertisement.
class ServerListService {
public:
    struct Sources {
        std::int32_t slots = 0;
        PlayerDirectory* players = nullptr;
    };

    [[nodiscard]] static std::expected<std::unique_ptr<ServerListService>, std::string>
    create(std::string_view configDirectory, Sources sources);

    [[nodiscard]] wire::protocol::status::ServerStatusPayload statusFor(std::int32_t protocol, std::string_view host,
                                                                        std::string_view address);

    /// First line of the winning profile's MOTD as plain text, fit for the Bedrock advertisement.
    [[nodiscard]] std::string bedrockMotd();

    void playerJoined(std::string_view uuid, std::string_view name);

    void playerLeft(std::string_view uuid);

    void setLoading(bool loading) { status_->setLoading(loading); }

    [[nodiscard]] bool isLoading() const { return status_->isLoading(); }

    [[nodiscard]] wire::common::placeholder::PlaceholderRegistry& placeholders() noexcept { return placeholders_; }

    [[nodiscard]] bool rememberIp() const;

    /// Reads server_list.toml again; a broken file leaves the current one in place.
    [[nodiscard]] std::expected<void, std::string> reload();

private:
    ServerListService(std::string directory, wire::config::ServerListConfig initial, Sources sources);

    std::string directory_;
    Sources sources_;
    wire::config::LiveValue<wire::config::ServerListConfig> config_;
    wire::common::placeholder::PlaceholderRegistry placeholders_;
    std::mutex rosterMutex_;
    std::map<std::string, std::string, std::less<>> roster_;
    std::unique_ptr<wire::config::ServerListStatus> status_;
};

} // namespace basalt::proxy::runtime::detail
