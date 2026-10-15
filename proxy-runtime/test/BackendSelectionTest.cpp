#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../src/BackendAttempts.hpp"
#include "../src/BackendDial.hpp"
#include "../src/BackendLogin.hpp"
#include "../src/BackendRoutes.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/io/BudgetMemoryResource.hpp>
#include <basalt/wire/io/EventLoop.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>

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

TEST_CASE("revisiting a backend never accepts callbacks from an earlier attempt") {
    BackendAttempts attempts;
    const auto firstA = attempts.begin(7);
    REQUIRE(firstA);
    CHECK(attempts.matches(*firstA));
    const auto b = attempts.begin(9);
    REQUIRE(b);
    CHECK_FALSE(attempts.matches(*firstA));
    CHECK(attempts.matches(*b));
    const auto secondA = attempts.begin(7);
    REQUIRE(secondA);
    CHECK(firstA->target == secondA->target);
    CHECK(firstA->generation != secondA->generation);
    CHECK_FALSE(attempts.matches(*firstA));
    CHECK_FALSE(attempts.matches(*b));
    CHECK_FALSE(attempts.matches({secondA->generation, b->target}));
    CHECK(attempts.matches(*secondA));
    attempts.clear();
    CHECK_FALSE(attempts.matches(*secondA));
    const auto thirdA = attempts.begin(7);
    REQUIRE(thirdA);
    CHECK_FALSE(attempts.matches(*secondA));
    CHECK(attempts.matches(*thirdA));
}

TEST_CASE("backend transport retains its own attempt and forwarding state across candidate replacement") {
    io::ResourceBudget global{2 * 1024 * 1024};
    io::ResourceBudget local{2 * 1024 * 1024};
    io::BudgetMemoryResource memory{global, local};
    basalt::wire::auth::SessionProfile profile;
    profile.name = "Tester";
    profile.id = basalt::wire::common::Uuid::offlinePlayer(profile.name);
    config::BackendConfig config;
    config.forwarding = basalt::wire::auth::forwarding::ForwardingMode::Velocity;
    config.secret = "fixture-secret";
    const auto forward = [&](BackendLogin& endpoint) {
        REQUIRE(endpoint.start(config, profile, "localhost", 25565, "127.0.0.1"));
        io::ByteBuffer query{64};
        query.writeBytes(std::array<std::uint8_t, 23>{42,  20,  'v', 'e', 'l', 'o', 'c', 'i', 't', 'y', ':', 'p',
                                                      'l', 'a', 'y', 'e', 'r', '_', 'i', 'n', 'f', 'o', 4});
        REQUIRE(endpoint.answerPluginQuery(query, config, profile, "127.0.0.1"));
    };
    BackendAttempts attempts;
    const auto firstA = attempts.begin(0);
    REQUIRE(firstA);
    {
        BackendLogin active{io::TcpConnection{}, memory, *firstA};
        forward(active);
        const auto b = attempts.begin(1);
        REQUIRE(b);
        {
            BackendLogin candidate{io::TcpConnection{}, memory, *b};
            CHECK(candidate.attempt() == *b);
            CHECK_FALSE(candidate.velocityForwarded());
            CHECK(active.attempt() == *firstA);
            CHECK(active.velocityForwarded());
            CHECK(local.used() >= active.allocated() + candidate.allocated());
        }
        const auto secondA = attempts.begin(0);
        REQUIRE(secondA);
        {
            BackendLogin replacement{io::TcpConnection{}, memory, *secondA};
            CHECK(replacement.attempt().target == active.attempt().target);
            CHECK(replacement.attempt().generation != active.attempt().generation);
            CHECK_FALSE(replacement.velocityForwarded());
            forward(replacement);
            attempts.clear();
            CHECK_FALSE(attempts.matches(replacement.attempt()));
            CHECK(replacement.attempt() == *secondA);
            CHECK(active.attempt() == *firstA);
            CHECK(active.velocityForwarded());
        }
        CHECK(local.used() >= active.allocated());
    }
    CHECK(local.used() == 0);
    CHECK(global.used() == 0);
}

TEST_CASE("active and candidate backend phases advance independently and reject out of order acknowledgements") {
    io::ResourceBudget global{64 * 1024};
    io::ResourceBudget local{64 * 1024};
    io::BudgetMemoryResource memory{global, local};
    {
        BackendLogin active{io::TcpConnection{}, memory, {1, 0}};
        BackendLogin candidate{io::TcpConnection{}, memory, {2, 1}};
        using Phase = BackendLogin::Phase;
        CHECK_FALSE(active.acknowledgeLogin());
        CHECK_FALSE(active.finishConfiguration());
        CHECK_FALSE(active.beginConfiguration());
        CHECK(active.phase() == Phase::kUnstarted);
        basalt::wire::auth::SessionProfile profile;
        profile.name = "Tester";
        profile.id = basalt::wire::common::Uuid::offlinePlayer(profile.name);
        config::BackendConfig config;
        const auto accept = [&](BackendLogin& endpoint) {
            io::ByteBuffer packet{128};
            basalt::wire::protocol::login::LoginFinishedPacket finished;
            finished.gameProfile.id = profile.id;
            finished.gameProfile.name = profile.name;
            finished.write(packet);
            return endpoint.acceptLogin(packet, config, profile);
        };
        CHECK_FALSE(accept(active));
        REQUIRE(active.start(config, profile, "localhost", 25565, "127.0.0.1"));
        CHECK(active.phase() == Phase::kLogin);
        CHECK_FALSE(active.acknowledgeLogin());
        REQUIRE(accept(active));
        REQUIRE(active.acknowledgeLogin());
        CHECK_FALSE(active.acknowledgeLogin());
        REQUIRE(active.finishConfiguration());
        REQUIRE(candidate.start(config, profile, "localhost", 25565, "127.0.0.1"));
        CHECK_FALSE(candidate.finishConfiguration());
        CHECK(candidate.phase() == Phase::kLogin);
        CHECK(active.phase() == Phase::kPlay);
        REQUIRE(accept(candidate));
        REQUIRE(candidate.acknowledgeLogin());
        CHECK(candidate.phase() == Phase::kConfiguration);
        CHECK(active.phase() == Phase::kPlay);
        REQUIRE(active.beginConfiguration());
        CHECK_FALSE(active.beginConfiguration());
        REQUIRE(active.finishConfiguration());
        CHECK(candidate.phase() == Phase::kConfiguration);
        REQUIRE(candidate.finishConfiguration());
        CHECK_FALSE(candidate.finishConfiguration());
        CHECK_FALSE(candidate.start(config, profile, "localhost", 25565, "127.0.0.1"));
        CHECK(active.phase() == Phase::kPlay);
        CHECK(candidate.phase() == Phase::kPlay);
    }
    CHECK(local.used() == 0);
    CHECK(global.used() == 0);
}

TEST_CASE("backend query failure cannot grant forwarding authority or exceed buffer quota") {
    bool malformed = false;
    bool wrongPhase = false;
    SUBCASE("forwarding output reservation fails under a small quota") {}
    SUBCASE("truncated channel is rejected before signing") { malformed = true; }
    SUBCASE("a login accepted backend cannot accept another login query") { wrongPhase = true; }
    io::ResourceBudget global{16 * 1024};
    io::ResourceBudget local{16 * 1024};
    io::BudgetMemoryResource memory{global, local};
    {
        BackendLogin endpoint{io::TcpConnection{}, memory, {1, 0}};
        basalt::wire::auth::SessionProfile profile;
        profile.name = "Tester";
        profile.id = basalt::wire::common::Uuid::offlinePlayer(profile.name);
        config::BackendConfig config;
        config.forwarding = wrongPhase ? basalt::wire::auth::forwarding::ForwardingMode::None
                                       : basalt::wire::auth::forwarding::ForwardingMode::Velocity;
        config.secret = "fixture-secret";
        REQUIRE(endpoint.start(config, profile, "localhost", 25565, "127.0.0.1"));
        io::ByteBuffer query{64};
        query.writeBytes(std::array<std::uint8_t, 23>{42,  20,  'v', 'e', 'l', 'o', 'c', 'i', 't', 'y', ':', 'p',
                                                      'l', 'a', 'y', 'e', 'r', '_', 'i', 'n', 'f', 'o', 4});
        if (malformed) {
            query.clear();
            query.writeBytes(std::array<std::uint8_t, 3>{42, 20, 'v'});
        }
        if (wrongPhase) {
            io::ByteBuffer login{128};
            basalt::wire::protocol::login::LoginFinishedPacket finished;
            finished.gameProfile.id = profile.id;
            finished.gameProfile.name = profile.name;
            finished.write(login);
            REQUIRE(endpoint.acceptLogin(login, config, profile));
        }
        const auto pending = endpoint.pendingBytes();
        const auto reserved = local.used();
        const auto response = endpoint.answerPluginQuery(query, config, profile, "127.0.0.1");
        REQUIRE_FALSE(response);
        CHECK(response.error() ==
              (malformed || wrongPhase ? BackendLoginError::kMalformed : BackendLoginError::kLimit));
        CHECK_FALSE(endpoint.velocityForwarded());
        CHECK(endpoint.pendingBytes() == pending);
        CHECK(local.used() == reserved);
        CHECK(endpoint.phase() == (wrongPhase ? BackendLogin::Phase::kLoginAccepted : BackendLogin::Phase::kLogin));
    }
    CHECK(local.used() == 0);
    CHECK(global.used() == 0);
}

TEST_CASE("backend identity validation preserves login phase on rejection and verified UUID on acceptance") {
    bool wrongName = false;
    bool wrongId = false;
    bool missingForwarding = false;
    bool trailing = false;
    SUBCASE("explicit none accepts offline backend identity and retains verified identity") {}
    SUBCASE("different name is rejected") { wrongName = true; }
    SUBCASE("unrelated UUID is rejected") { wrongId = true; }
    SUBCASE("Velocity requires its own signed reply") { missingForwarding = true; }
    SUBCASE("trailing payload is rejected") { trailing = true; }
    io::ResourceBudget global{64 * 1024};
    io::ResourceBudget local{64 * 1024};
    io::BudgetMemoryResource memory{global, local};
    {
        BackendLogin endpoint{io::TcpConnection{}, memory, {1, 0}};
        basalt::wire::auth::SessionProfile profile;
        profile.name = "Tester";
        profile.id = basalt::wire::common::Uuid::offlinePlayer("VerifiedIdentity");
        config::BackendConfig config;
        if (missingForwarding) {
            config.forwarding = basalt::wire::auth::forwarding::ForwardingMode::Velocity;
            config.secret = "fixture-secret";
        }
        REQUIRE(endpoint.start(config, profile, "localhost", 25565, "127.0.0.1"));
        basalt::wire::protocol::login::LoginFinishedPacket finished;
        finished.gameProfile.name = wrongName ? "Other" : profile.name;
        finished.gameProfile.id = basalt::wire::common::Uuid::offlinePlayer(wrongId ? "Other" : profile.name);
        finished.sessionId = basalt::wire::common::Uuid::offlinePlayer("BackendSession");
        io::ByteBuffer packet{128};
        finished.write(packet);
        if (trailing)
            packet.writeByte(0);
        const auto accepted = endpoint.acceptLogin(packet, config, profile);
        if (wrongName || wrongId || missingForwarding || trailing) {
            CHECK_FALSE(accepted);
            CHECK(endpoint.phase() == BackendLogin::Phase::kLogin);
        } else {
            REQUIRE(accepted);
            CHECK(accepted->gameProfile.id == profile.id);
            CHECK(accepted->sessionId == finished.sessionId);
            CHECK(endpoint.phase() == BackendLogin::Phase::kLoginAccepted);
        }
    }
    CHECK(global.used() == 0);
    CHECK(local.used() == 0);
}
