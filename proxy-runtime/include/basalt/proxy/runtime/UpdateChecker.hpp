#pragma once

#include <basalt/wire/common/logging/Logger.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime::updateChecker {

enum class Channel : std::uint8_t { Release, Nightly };

struct ParsedVersion {
    std::vector<std::int32_t> base;
    std::optional<std::int32_t> nightly;
    std::string raw;

    [[nodiscard]] bool isNightly() const noexcept { return nightly.has_value(); }

    [[nodiscard]] bool sameMinecraftSeries(const ParsedVersion& other) const noexcept;

    [[nodiscard]] std::int32_t patch() const noexcept { return base.empty() ? 0 : base.back(); }

    friend bool operator==(const ParsedVersion&, const ParsedVersion&) = default;
};

struct UpdateNotice {
    Channel channel = Channel::Release;
    std::string currentVersion;
    std::string latestVersion;
    std::int32_t buildsBehind = 0;
    std::int32_t releasesBehind = 0;
    std::string url;

    [[nodiscard]] std::string message() const;
};

using Fetch = std::function<std::optional<std::string>(std::string_view url)>;

[[nodiscard]] std::optional<ParsedVersion> parseVersion(std::string_view text);

[[nodiscard]] std::optional<UpdateNotice> evaluate(std::string_view currentVersion, std::string_view releasesJson,
                                                   std::string_view nightlyJson = {});

[[nodiscard]] std::optional<UpdateNotice> check(std::string_view currentVersion, const Fetch& fetch);

void checkAndAnnounce(std::string_view currentVersion, const wire::common::Logger& logger, std::stop_token stop = {},
                      const Fetch& fetch = {});

} // namespace basalt::proxy::runtime::updateChecker
