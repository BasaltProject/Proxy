#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/common/toml/Toml.hpp>
#include <basalt/wire/io/FileIO.hpp>

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <string>

using namespace basalt::proxy::config;
namespace toml = basalt::wire::common::toml;
namespace fileIO = basalt::wire::io::fileIO;

namespace {
std::expected<ProxyConfig, std::string> decode(std::string_view proxy,
                                               std::string_view backends = defaultBackendsToml()) {
    auto p = toml::parse(proxy);
    auto b = toml::parse(backends);
    if (!p || !b)
        return std::unexpected("synthetic fixture did not parse");
    return fromToml(*p, *b, "config");
}
} // namespace

TEST_CASE("commented defaults produce online authentication, bounded owners "
          "and explicit backend identity") {
    const auto defaults = decode(defaultProxyToml());
    REQUIRE(defaults);
    CHECK(defaults->network.onlineMode);
    CHECK(defaults->runtime.ioLoops > 1);
    CHECK(defaults->runtime.maxBufferedBytes >= defaults->runtime.sessionBufferBytes);
    CHECK(defaults->translation == TranslationMode::Auto);
    REQUIRE(defaults->backends.size() == 1);
    CHECK(defaults->backends.front().forwarding == basalt::wire::auth::forwarding::ForwardingMode::None);
    CHECK(defaults->defaultRoute == std::vector<std::string>{"lobby"});
}

TEST_CASE("invalid network and resource policy fails before opening sockets") {
    for (auto document : {"[network]\nport = -1", "[network]\nport = 65536", "[network]\nport = true",
                          "[network]\nonline-mode = 'false'", "[network]\nbind = 'hostname.test'",
                          "[network]\ndual-stack = true", "[network]\nonline_mod = false", "[runtime]\nio-loops = 0",
                          "[runtime]\nworkers = 65", "[runtime]\nmax-buffered-bytes = 4096",
                          "[runtime]\nlow-water-bytes = 4194304", "[runtime]\nlogin-timeout-ms = 0",
                          "[admission]\nmax-origins = -1", "[translation]\nmode = 'server'", "network = false"}) {
        CAPTURE(document);
        const auto result = decode(document);
        REQUIRE_FALSE(result);
        CHECK(result.error().starts_with("config/proxy.toml:"));
    }
}

TEST_CASE("backend identity requires the configured secret without exposing "
          "its value in errors") {
    const auto unspecified = decode("", "[routing]\ndefault = ['lobby']\n[backends.lobby]\nhost = 'localhost'\n");
    REQUIRE_FALSE(unspecified);
    CHECK(unspecified.error().find("forwarding must be chosen explicitly") != std::string::npos);
    for (auto mode : {"velocity", "bungeeguard", "safenet"}) {
        const std::string text = "[routing]\ndefault = ['lobby']\n[backends.lobby]\nhost = "
                                 "'localhost'\nforwarding = '" +
                                 std::string{mode} + "'\n";
        CHECK_FALSE(decode("", text));
        auto secret = decode("", text + "secret = 'synthetic-secret'\n");
        REQUIRE(secret);
        CHECK(secret->backends.front().secret == "synthetic-secret");
    }
    const auto invalid = decode("", "[routing]\ndefault = ['lobby']\n[backends.lobby]\nhost = 'localhost'\n"
                                    "forwarding = 'none'\nsecret = 'must-not-appear'\n");
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().find("must-not-appear") == std::string::npos);
    CHECK(invalid.error().find("secret") != std::string::npos);
}

TEST_CASE("routes preserve fallback order and normalize virtual hosts across "
          "mixed backend versions") {
    const auto result = decode("[network]\nbind = '::'\ndual-stack = "
                               "true\n[translation]\nmode = 'proxy'",
                               R"(
[routing]
default = ['old', 'new']
[forced-hosts]
"PLAY.Example.test." = ['new', 'old']
[backends.old]
host = '::1'
protocol = 47
forwarding = 'bungeecord'
[backends.new]
host = '127.0.0.1'
protocol = 777
forwarding = 'velocity'
secret = 'synthetic-secret'
)");
    REQUIRE(result);
    CHECK(result->network.dualStack);
    CHECK(result->translation == TranslationMode::Proxy);
    CHECK(result->defaultRoute == std::vector<std::string>{"old", "new"});
    REQUIRE(result->forcedHosts.size() == 1);
    CHECK(result->forcedHosts.front().host == "play.example.test");
    CHECK(result->forcedHosts.front().backends == std::vector<std::string>{"new", "old"});
}

TEST_CASE("unknown, duplicate and mixed-type routing targets fail closed") {
    for (auto route : {"['missing']", "['lobby', 'lobby']", "['lobby', 3]", "[]", "'lobby'"}) {
        const auto result =
            decode("", "[routing]\ndefault = " + std::string{route} + "\n[backends.lobby]\nhost = '127.0.0.1'\n");
        REQUIRE_FALSE(result);
        CHECK(result.error().starts_with("config/backends.toml:"));
    }
    CHECK_FALSE(decode("", "[routing]\ndefault = ['lobby']\n[backends.lobby]\nhost = "
                           "'localhost:25565'"));
    CHECK_FALSE(decode("", "[routing]\ndefault = ['lobby']\n[backends.lobby]\nhost = 'localhost'\n"
                           "[forced-hosts]\n'PLAY.test' = ['lobby']\n'play.test' = ['lobby']"));
}

TEST_CASE("loading creates domain files and preserves explicit offline "
          "operator settings") {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("basalt-proxy-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    struct Cleanup {
        std::filesystem::path root;

        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } cleanup{root};

    auto first = load(root.string());
    REQUIRE(first);
    const auto written = fileIO::readText((root / "proxy.toml").string());
    REQUIRE(written);
    CHECK(written->find('#') != std::string::npos);
    REQUIRE(fileIO::writeText((root / "proxy.toml").string(), "[network]\nonline-mode = false\n"));
    auto edited = load(root.string());
    REQUIRE(edited);
    CHECK_FALSE(edited->network.onlineMode);
    REQUIRE(fileIO::writeText((root / "backends.toml").string(), "[broken"));
    const auto broken = load(root.string());
    REQUIRE_FALSE(broken);
    CHECK(broken.error().find("backends.toml is not valid TOML") != std::string::npos);
}
