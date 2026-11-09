#include <basalt/proxy/runtime/UpdateChecker.hpp>
#include <basalt/wire/auth/HttpsClient.hpp>
#include <basalt/wire/common/json/Json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>

namespace basalt::proxy::runtime::updateChecker {
namespace {

constexpr std::string_view kReleasesApiUrl = "https://api.github.com/repos/BasaltProject/Proxy/releases?per_page=100";
constexpr std::string_view kNightlyApiUrl = "https://api.github.com/repos/BasaltProject/Proxy/releases/tags/nightly";
constexpr std::string_view kLatestReleasePageUrl = "https://github.com/BasaltProject/Proxy/releases/latest";
constexpr std::string_view kNightlyReleasePageUrl = "https://github.com/BasaltProject/Proxy/releases/tag/nightly";

constexpr std::size_t kMaxReleasesJsonBytes = 4UL * 1024UL * 1024UL;
constexpr auto kRequestTimeout = std::chrono::seconds{10};

[[nodiscard]] std::string_view trimAscii(std::string_view text) noexcept {
    while (!text.empty() && static_cast<unsigned char>(text.front()) <= ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && static_cast<unsigned char>(text.back()) <= ' ') {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::optional<std::int32_t> parseNumber(std::string_view text) noexcept {
    if (text.empty() || text.front() < '0' || text.front() > '9') {
        return std::nullopt;
    }
    std::int32_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value < 0) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::vector<std::int32_t>> parseBaseNumbers(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::vector<std::int32_t> numbers;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t dot = std::min(text.find('.', start), text.size());
        const std::optional<std::int32_t> part = parseNumber(text.substr(start, dot - start));
        if (!part.has_value()) {
            return std::nullopt;
        }
        numbers.push_back(*part);
        if (dot == text.size()) {
            break;
        }
        start = dot + 1;
    }
    if (numbers.size() < 3 || numbers.front() <= 0) {
        return std::nullopt;
    }
    return numbers;
}

[[nodiscard]] std::string_view stringField(const wire::common::JsonObject& object, std::string_view key) {
    const wire::common::JsonValue* value = object.find(key);
    const std::string* text = value == nullptr ? nullptr : value->get<std::string>();
    return text == nullptr ? std::string_view{} : std::string_view{*text};
}

[[nodiscard]] bool boolField(const wire::common::JsonObject& object, std::string_view key) {
    const wire::common::JsonValue* value = object.find(key);
    const bool* flag = value == nullptr ? nullptr : value->get<bool>();
    return flag != nullptr && *flag;
}

[[nodiscard]] std::optional<ParsedVersion> nightlyFromAssets(const wire::common::JsonObject& release) {
    const wire::common::JsonValue* assetsValue = release.find("assets");
    const auto* assets = assetsValue == nullptr ? nullptr : assetsValue->get<std::vector<wire::common::JsonValue>>();
    if (assets == nullptr) {
        return std::nullopt;
    }
    constexpr std::string_view kPrefix = "BasaltProxy-";
    for (const wire::common::JsonValue& entry : *assets) {
        const auto* asset = entry.get<wire::common::JsonObject>();
        if (asset == nullptr) {
            continue;
        }
        std::string_view name = stringField(*asset, "name");
        if (!name.starts_with(kPrefix)) {
            continue;
        }
        name.remove_prefix(kPrefix.size());
        const std::size_t firstDash = name.find('-');
        if (firstDash == std::string_view::npos) {
            continue;
        }
        const std::size_t secondDash = name.find('-', firstDash + 1);
        if (secondDash == std::string_view::npos) {
            continue;
        }
        if (const std::optional<ParsedVersion> parsed = parseVersion(name.substr(0, secondDash));
            parsed.has_value() && parsed->isNightly()) {
            return parsed;
        }
    }
    return std::nullopt;
}

struct Catalog {
    std::vector<ParsedVersion> releases;
    std::optional<ParsedVersion> nightly;
};

void collectReleaseObject(const wire::common::JsonObject& object, Catalog& catalog) {
    if (boolField(object, "draft")) {
        return;
    }
    const std::string_view tag = stringField(object, "tag_name");
    const std::string_view title = stringField(object, "name");
    const bool prerelease = boolField(object, "prerelease");

    if (tag == "nightly" || prerelease) {
        std::optional<ParsedVersion> parsed = parseVersion(title);
        if (!parsed.has_value() || !parsed->isNightly()) {
            parsed = nightlyFromAssets(object);
        }
        if (parsed.has_value() && parsed->isNightly()) {
            if (!catalog.nightly.has_value() || catalog.nightly->base < parsed->base ||
                (catalog.nightly->base == parsed->base && *catalog.nightly->nightly < *parsed->nightly)) {
                catalog.nightly = std::move(*parsed);
            }
        }
        return;
    }

    std::optional<ParsedVersion> parsed = parseVersion(tag);
    if (!parsed.has_value() || parsed->isNightly()) {
        parsed = parseVersion(title);
    }
    if (parsed.has_value() && !parsed->isNightly()) {
        catalog.releases.push_back(std::move(*parsed));
    }
}

void parseCatalogInto(std::string_view jsonText, Catalog& catalog) {
    if (jsonText.empty()) {
        return;
    }
    const std::expected<wire::common::JsonValue, wire::common::JsonError> parsed = wire::common::parseJson(jsonText);
    if (!parsed.has_value()) {
        return;
    }
    if (const auto* list = parsed->get<std::vector<wire::common::JsonValue>>(); list != nullptr) {
        for (const wire::common::JsonValue& entry : *list) {
            if (const auto* object = entry.get<wire::common::JsonObject>(); object != nullptr) {
                collectReleaseObject(*object, catalog);
            }
        }
    } else if (const auto* object = parsed->get<wire::common::JsonObject>(); object != nullptr) {
        collectReleaseObject(*object, catalog);
    }
}

} // namespace

bool ParsedVersion::sameMinecraftSeries(const ParsedVersion& other) const noexcept {
    if (base.size() != other.base.size() || base.size() < 2) {
        return false;
    }
    return std::equal(base.begin(), base.end() - 1, other.base.begin());
}

std::string UpdateNotice::message() const {
    if (channel == Channel::Nightly) {
        std::string behind;
        if (releasesBehind > 0) {
            behind = std::to_string(releasesBehind) + (releasesBehind == 1 ? " release and " : " releases and ");
        }
        behind +=
            std::to_string(buildsBehind) + (buildsBehind == 1 ? " nightly build behind" : " nightly builds behind");
        return "A new nightly build of Basalt Proxy is available: " + latestVersion + " (running " + currentVersion +
               ", " + behind +
               "). Updating is important to stay up to date with bug fixes, features and security fixes: " + url;
    }
    const std::string behind =
        std::to_string(buildsBehind) + (buildsBehind == 1 ? " release behind" : " releases behind");
    return "A new release of Basalt Proxy is available: " + latestVersion + " (running " + currentVersion + ", " +
           behind + "). Updating is important to stay up to date with bug fixes, features and security fixes: " + url;
}

std::optional<ParsedVersion> parseVersion(std::string_view text) {
    text = trimAscii(text);
    if (text.starts_with("Nightly ") || text.starts_with("nightly ")) {
        text = trimAscii(text.substr(8));
    }
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    const std::size_t dash = text.find('-');
    if (dash == std::string_view::npos) {
        std::optional<std::vector<std::int32_t>> base = parseBaseNumbers(text);
        if (!base.has_value() || base->back() <= 0) {
            return std::nullopt;
        }
        return ParsedVersion{.base = std::move(*base), .nightly = std::nullopt, .raw = std::string{text}};
    }

    const std::string_view basePart = text.substr(0, dash);
    const std::string_view suffixPart = text.substr(dash + 1);
    if (suffixPart.size() < 2 || suffixPart.front() != 'n') {
        return std::nullopt;
    }
    const std::optional<std::int32_t> nightlyCount = parseNumber(suffixPart.substr(1));
    if (!nightlyCount.has_value() || *nightlyCount <= 0) {
        return std::nullopt;
    }
    std::optional<std::vector<std::int32_t>> base = parseBaseNumbers(basePart);
    if (!base.has_value()) {
        return std::nullopt;
    }
    return ParsedVersion{.base = std::move(*base), .nightly = *nightlyCount, .raw = std::string{text}};
}

std::optional<UpdateNotice> evaluate(std::string_view currentVersion, std::string_view releasesJson,
                                     std::string_view nightlyJson) {
    const std::optional<ParsedVersion> current = parseVersion(currentVersion);
    if (!current.has_value()) {
        return std::nullopt;
    }

    Catalog catalog;
    parseCatalogInto(releasesJson, catalog);
    parseCatalogInto(nightlyJson, catalog);

    std::ranges::sort(catalog.releases,
                      [](const ParsedVersion& left, const ParsedVersion& right) { return left.base < right.base; });
    const auto deduplicated =
        std::ranges::unique(catalog.releases, [](const ParsedVersion& left, const ParsedVersion& right) {
            return left.base == right.base;
        });
    catalog.releases.erase(deduplicated.begin(), deduplicated.end());

    if (!current->isNightly()) {
        std::vector<ParsedVersion> newer;
        for (const ParsedVersion& release : catalog.releases) {
            if (release.base > current->base) {
                newer.push_back(release);
            }
        }
        if (newer.empty()) {
            return std::nullopt;
        }
        const ParsedVersion& latest = newer.back();
        std::int32_t behind = static_cast<std::int32_t>(newer.size());
        if (latest.sameMinecraftSeries(*current)) {
            behind = std::max(behind, latest.patch() - current->patch());
        }
        return UpdateNotice{
            .channel = Channel::Release,
            .currentVersion = current->raw,
            .latestVersion = latest.raw,
            .buildsBehind = behind,
            .releasesBehind = behind,
            .url = std::string{kLatestReleasePageUrl},
        };
    }

    if (!catalog.nightly.has_value()) {
        return std::nullopt;
    }
    const ParsedVersion& latestNightly = *catalog.nightly;
    if (latestNightly.base < current->base) {
        return std::nullopt;
    }
    if (latestNightly.base == current->base) {
        if (*latestNightly.nightly <= *current->nightly) {
            return std::nullopt;
        }
        const std::int32_t behind = *latestNightly.nightly - *current->nightly;
        return UpdateNotice{
            .channel = Channel::Nightly,
            .currentVersion = current->raw,
            .latestVersion = latestNightly.raw,
            .buildsBehind = behind,
            .releasesBehind = 0,
            .url = std::string{kNightlyReleasePageUrl},
        };
    }

    std::int32_t releasesBetween = 0;
    for (const ParsedVersion& release : catalog.releases) {
        if (release.base > current->base && release.base <= latestNightly.base) {
            ++releasesBetween;
        }
    }
    if (latestNightly.sameMinecraftSeries(*current)) {
        releasesBetween = std::max(releasesBetween, latestNightly.patch() - current->patch());
    } else {
        releasesBetween = std::max(releasesBetween, latestNightly.patch());
    }
    return UpdateNotice{
        .channel = Channel::Nightly,
        .currentVersion = current->raw,
        .latestVersion = latestNightly.raw,
        .buildsBehind = *latestNightly.nightly,
        .releasesBehind = releasesBetween,
        .url = std::string{kNightlyReleasePageUrl},
    };
}

std::optional<UpdateNotice> check(std::string_view currentVersion, const Fetch& fetch) {
    const std::optional<ParsedVersion> current = parseVersion(currentVersion);
    if (!current.has_value() || !fetch) {
        return std::nullopt;
    }
    const std::optional<std::string> releasesJson = fetch(kReleasesApiUrl);
    if (!releasesJson.has_value()) {
        return std::nullopt;
    }
    if (!current->isNightly()) {
        return evaluate(currentVersion, *releasesJson);
    }
    Catalog catalog;
    parseCatalogInto(*releasesJson, catalog);
    std::string nightlyJson;
    if (!catalog.nightly.has_value()) {
        if (const std::optional<std::string> fetchedNightly = fetch(kNightlyApiUrl); fetchedNightly.has_value()) {
            nightlyJson = *fetchedNightly;
        }
    }
    return evaluate(currentVersion, *releasesJson, nightlyJson);
}

void checkAndAnnounce(std::string_view currentVersion, const wire::common::Logger& logger, std::stop_token stop,
                      const Fetch& fetch) {
    if (!parseVersion(currentVersion).has_value() || stop.stop_requested()) {
        return;
    }
    const Fetch getter = fetch ? fetch : Fetch{[&stop](std::string_view url) -> std::optional<std::string> {
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        const std::expected<wire::auth::HttpsResponse, wire::auth::HttpsError> response = wire::auth::httpsGetBounded(
            url, kMaxReleasesJsonBytes,
            wire::auth::BoundedHttpsOptions{.deadline = std::chrono::steady_clock::now() + kRequestTimeout,
                                            .stop = stop});
        if (!response.has_value() || !response->ok()) {
            return std::nullopt;
        }
        return response->text();
    }};
    const std::optional<UpdateNotice> notice = check(currentVersion, getter);
    if (!notice.has_value() || stop.stop_requested()) {
        return;
    }
    logger.warn([&notice] { return notice->message(); });
}

} // namespace basalt::proxy::runtime::updateChecker
