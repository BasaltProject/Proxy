#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <basalt/proxy/runtime/Cli.hpp>
#include <basalt/proxy/runtime/UpdateChecker.hpp>
#include <basalt/wire/common/logging/LogOutput.hpp>

#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace basalt::proxy::runtime;

namespace {

class CapturedLog {
public:
    CapturedLog() {
        basalt::wire::common::logOutput::addSink([this](std::string_view line) { lines_.emplace_back(line); });
    }

    CapturedLog(const CapturedLog&) = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;

    ~CapturedLog() { basalt::wire::common::logOutput::clearSinks(); }

    [[nodiscard]] bool saidSomethingWith(std::string_view fragment) const {
        return std::ranges::any_of(
            lines_, [fragment](const std::string& line) { return line.find(fragment) != std::string::npos; });
    }

    [[nodiscard]] std::size_t size() const noexcept { return lines_.size(); }

private:
    std::vector<std::string> lines_;
};

} // namespace

TEST_CASE("no arguments runs with the default configuration directory") {
    const CliCommand parsed = cli::parse({});
    const auto* run = std::get_if<cliCommand::Run>(&parsed);
    REQUIRE(run != nullptr);
    CHECK(run->configDirectory == cli::kDefaultConfigDirectory);
}

TEST_CASE("a config without a value is rejected rather than defaulted") {
    CHECK(std::holds_alternative<cliCommand::Invalid>(cli::parse({"--config"})));
    CHECK(std::holds_alternative<cliCommand::Invalid>(cli::parse({"--config", "--version"})));
}

TEST_CASE("update checker compares releases and nightlies on their own channels") {
    namespace uc = basalt::proxy::runtime::updateChecker;

    CHECK_FALSE(uc::parseVersion("0.0.0-dev").has_value());
    CHECK_FALSE(uc::parseVersion("26.3.0-dev").has_value());
    CHECK_FALSE(uc::parseVersion("26.3.2-ci18").has_value());
    CHECK_FALSE(uc::parseVersion("26.3.0").has_value());
    CHECK_FALSE(uc::parseVersion("26.3.2-n0").has_value());

    const auto rel = uc::parseVersion("v26.3.2");
    REQUIRE(rel.has_value());
    CHECK_FALSE(rel->isNightly());
    CHECK(rel->base == std::vector<std::int32_t>{26, 3, 2});
    CHECK(rel->raw == "26.3.2");

    const auto nightly = uc::parseVersion("Nightly 26.3.2-n5");
    REQUIRE(nightly.has_value());
    CHECK(nightly->isNightly());
    CHECK(nightly->base == std::vector<std::int32_t>{26, 3, 2});
    CHECK(nightly->nightly == 5);
    CHECK(nightly->raw == "26.3.2-n5");

    constexpr std::string_view kReleasesJson = R"([
        {"tag_name": "v26.3.3", "name": "26.3.3", "prerelease": false, "draft": false},
        {"tag_name": "v26.3.2", "name": "26.3.2", "prerelease": false, "draft": false},
        {"tag_name": "v26.3.1", "name": "26.3.1", "prerelease": false, "draft": false},
        {"tag_name": "nightly", "name": "Nightly 26.3.3-n4", "prerelease": true, "draft": false}
    ])";

    CHECK_FALSE(uc::evaluate("26.3.3", kReleasesJson).has_value());
    CHECK_FALSE(uc::evaluate("26.3.0-dev", kReleasesJson).has_value());
    CHECK_FALSE(uc::evaluate("26.3.1-ci4", kReleasesJson).has_value());

    const auto releaseBehind = uc::evaluate("26.3.1", kReleasesJson);
    REQUIRE(releaseBehind.has_value());
    CHECK(releaseBehind->channel == uc::Channel::Release);
    CHECK(releaseBehind->latestVersion == "26.3.3");
    CHECK(releaseBehind->buildsBehind == 2);
    CHECK(releaseBehind->message().find("2 releases behind") != std::string::npos);
    CHECK(releaseBehind->message().find("bug fixes, features and security fixes") != std::string::npos);

    const auto nightlySameBase = uc::evaluate("26.3.3-n1", kReleasesJson);
    REQUIRE(nightlySameBase.has_value());
    CHECK(nightlySameBase->channel == uc::Channel::Nightly);
    CHECK(nightlySameBase->latestVersion == "26.3.3-n4");
    CHECK(nightlySameBase->buildsBehind == 3);
    CHECK(nightlySameBase->releasesBehind == 0);
    CHECK(nightlySameBase->message().find("3 nightly builds behind") != std::string::npos);

    CHECK_FALSE(uc::evaluate("26.3.3-n4", kReleasesJson).has_value());

    const auto nightlyOlderBase = uc::evaluate("26.3.2-n2", kReleasesJson);
    REQUIRE(nightlyOlderBase.has_value());
    CHECK(nightlyOlderBase->channel == uc::Channel::Nightly);
    CHECK(nightlyOlderBase->latestVersion == "26.3.3-n4");
    CHECK(nightlyOlderBase->releasesBehind == 1);
    CHECK(nightlyOlderBase->buildsBehind == 4);
    CHECK(nightlyOlderBase->message().find("1 release and 4 nightly builds behind") != std::string::npos);

    {
        const CapturedLog log;
        uc::checkAndAnnounce("26.3.3", basalt::wire::common::Logger{"Proxy"}, {},
                             [kReleasesJson](std::string_view) { return std::optional<std::string>{kReleasesJson}; });
        CHECK(log.size() == 0);
    }
    {
        const CapturedLog log;
        uc::checkAndAnnounce("26.3.2", basalt::wire::common::Logger{"Proxy"}, {},
                             [kReleasesJson](std::string_view) { return std::optional<std::string>{kReleasesJson}; });
        CHECK(log.saidSomethingWith("1 release behind"));
    }
    if (std::getenv("BASALT_TEST_LIVE_UPDATE_CHECK") != nullptr) {
        const CapturedLog releaseLog;
        uc::checkAndAnnounce("26.2.1", basalt::wire::common::Logger{"Proxy"});
        CHECK(releaseLog.saidSomethingWith("A new release of Basalt Proxy is available:"));

        const CapturedLog nightlyLog;
        uc::checkAndAnnounce("26.3.1-n1", basalt::wire::common::Logger{"Proxy"});
        CHECK(nightlyLog.saidSomethingWith("A new nightly build of Basalt Proxy is available:"));
    }
}
