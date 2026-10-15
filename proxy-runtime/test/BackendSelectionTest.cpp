#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../src/BackendDial.hpp"
#include "../src/BackendRoutes.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/io/EventLoop.hpp>

#include <doctest.h>

#include <array>

using namespace basalt::proxy;
using namespace runtime::detail;
namespace io = basalt::wire::io;
using namespace std::chrono_literals;

TEST_CASE("named and virtual-host routing retain explicit fallback order") {
    config::ProxyConfig config;
    config.backends = {{.name = "lobby", .host = "127.0.0.1"}, {.name = "game", .host = "127.0.0.1"}};
    config.defaultRoute = {"lobby", "game"};
    config.forcedHosts = {{"PLAY.Example.test.", {"game", "lobby"}}};
    auto routes = BackendRoutes::create(config);
    REQUIRE(routes);
    CHECK(routes->named("game") == 1);
    CHECK_FALSE(routes->named("missing"));
    const auto forced = routes->select("play.example.TEST");
    REQUIRE(forced.size() == 2);
    CHECK(forced[0] == 1);
    CHECK(forced[1] == 0);
    const auto defaults = routes->select("another.example.test");
    REQUIRE(defaults.size() == 2);
    CHECK(defaults[0] == 0);
    CHECK(defaults[1] == 1);
    CHECK(routes->select(std::string{"play\0.example", 13}).empty());
    config.defaultRoute = {"missing"};
    CHECK_FALSE(BackendRoutes::create(config));
    config.defaultRoute = {"lobby", "lobby"};
    CHECK_FALSE(BackendRoutes::create(config));
}

TEST_CASE("nonblocking backend dial advances from a refused endpoint to a live endpoint") {
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto live = io::TcpListener::bind(*address);
    auto closed = io::TcpListener::bind(*address);
    REQUIRE(live);
    REQUIRE(closed);
    const auto livePort = live->localPort();
    const auto closedPort = closed->localPort();
    REQUIRE(livePort);
    REQUIRE(closedPort);
    closed->close();
    auto first = io::TcpAddress::literal("127.0.0.1", *closedPort);
    auto second = io::TcpAddress::literal("127.0.0.1", *livePort);
    REQUIRE(first);
    REQUIRE(second);
    BackendDial dial{{*first, *second}, BackendDial::Clock::now() + 1s};
    io::EventLoop loop;
    auto state = dial.advance();
    REQUIRE(state);
    while (*state == DialState::kConnecting) {
        const auto handle = dial.handle();
        REQUIRE(loop.watch(handle, {false, true}, 1));
        auto ready = loop.wait(1000);
        REQUIRE(ready);
        REQUIRE_FALSE(ready->empty());
        loop.unwatch(handle);
        state = dial.advance();
        REQUIRE(state);
    }
    auto connected = dial.take();
    REQUIRE(connected);
    REQUIRE(connected->setNonBlocking());
    auto accepted = live->accept();
    REQUIRE(accepted);
    const std::array<std::uint8_t, 3> bytes{1, 2, 3};
    REQUIRE(accepted->write(bytes));
    REQUIRE(loop.watch(connected->nativeHandle(), {true, false}, 2));
    REQUIRE(loop.wait(1000));
    std::array<std::uint8_t, 3> received{};
    const auto read = connected->read(received);
    REQUIRE(read);
    CHECK(*read == bytes.size());
    CHECK(received == bytes);
    CHECK_FALSE(dial.take());
}

TEST_CASE("backend dial honors deadline/cancellation before creating or retaining a socket") {
    const auto endpoint = io::TcpAddress::literal("127.0.0.1", 1);
    REQUIRE(endpoint);
    BackendDial expired{{*endpoint}, BackendDial::Clock::now()};
    const auto timeout = expired.advance();
    REQUIRE_FALSE(timeout);
    CHECK(timeout.error().kind == DialFailure::kTimedOut);
    CHECK(expired.handle() == -1);
    std::stop_source stop;
    stop.request_stop();
    BackendDial canceled{{*endpoint}, BackendDial::Clock::now() + 1s, stop.get_token()};
    const auto result = canceled.advance();
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == DialFailure::kCanceled);
    CHECK(canceled.handle() == -1);
    BackendDial empty{{}, BackendDial::Clock::now() + 1s};
    CHECK_FALSE(empty.advance());
}
