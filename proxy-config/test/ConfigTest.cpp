#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <basalt/proxy/config/ConfigDefaults.hpp>
#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/common/toml/Toml.hpp>

#include <doctest.h>

#include <string>

using namespace basalt::proxy::config;
namespace toml = basalt::wire::common::toml;

namespace {
std::expected<ProxyConfig, std::string> decode(std::string_view proxy,
                                               std::string_view backends = kBackendsDefaultToml) {
    auto p = toml::parse(proxy);
    auto b = toml::parse(backends);
    if (!p || !b)
        return std::unexpected("synthetic fixture did not parse");
    return fromToml(*p, *b, "config");
}
} // namespace

TEST_CASE("commented defaults produce online authentication, bounded owners "
          "and explicit backend identity") {
    const auto defaults = decode(kProxyDefaultToml);
    REQUIRE(defaults);
    CHECK(defaults->network.onlineMode);
    CHECK_FALSE(defaults->network.acceptTransfers);
    CHECK(defaults->runtime.ioLoops > 1);
    CHECK(defaults->runtime.maxBufferedBytes >= defaults->runtime.sessionBufferBytes);
    CHECK(defaults->translation == TranslationMode::Auto);
    REQUIRE(defaults->backends.size() == 1);
    CHECK(defaults->backends.front().forwarding == basalt::wire::auth::forwarding::ForwardingMode::None);
    CHECK_FALSE(defaults->backends.front().bungeeMessages);
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

TEST_CASE("protocol auto lets a Basalt backend translate and keeps the proxy translating for the others") {
    const auto result = decode("", R"(
[routing]
default = ['basalt', 'other']
[backends.basalt]
host = '127.0.0.1'
protocol = 'auto'
forwarding = 'basalt'
[backends.other]
host = '127.0.0.2'
protocol = 'auto'
forwarding = 'velocity'
secret = 'synthetic-secret'
)");
    REQUIRE(result);
    CHECK(result->backends[0].protocol == BackendConfig::kClientProtocol);
    CHECK(result->backends[1].protocol == BackendConfig{}.protocol);
}
