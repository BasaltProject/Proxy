#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../src/BackendAttempts.hpp"
#include "../src/BackendDial.hpp"
#include "../src/BackendLogin.hpp"
#include "../src/BackendRoutes.hpp"
#include "../src/IdRenumbering.hpp"
#include "../src/PendingResponses.hpp"
#include "../src/RouteVersions.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/io/BudgetMemoryResource.hpp>
#include <basalt/wire/io/EventLoop.hpp>
#include <basalt/wire/multiversion/ProtocolVersions.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>

#include <doctest.h>

#include <array>

using namespace basalt::proxy;
using namespace runtime::detail;
namespace io = basalt::wire::io;
using namespace std::chrono_literals;

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
    BackendDial dial{{*first, *second}, BackendDial::Clock::now() + 10s};
    io::EventLoop loop;
    auto state = dial.advance();
    REQUIRE(state);
    while (*state == DialState::kConnecting) {
        const auto handle = dial.handle();
        REQUIRE(loop.watch(handle, {false, true}, 1));
        auto ready = loop.wait(5000);
        REQUIRE(ready);
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
        const auto accepted = endpoint.acceptLogin(packet);
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

TEST_CASE("clearing keepalives cannot alias a later backend's token") {
    io::ResourceBudget global{4096};
    io::ResourceBudget local{4096};
    io::BudgetMemoryResource memory{global, local};
    PendingResponses pending{memory};
    const auto first = pending.publish(-42, {1, 0});
    REQUIRE(first);
    pending.clear();
    const BackendAttempt replacement{2, 1};
    const auto second = pending.publish(-42, replacement);
    REQUIRE(second);
    CHECK(*first != *second);
    CHECK_FALSE(pending.consume(*first));
    const auto origin = pending.consume(*second);
    REQUIRE(origin);
    CHECK(origin->token == -42);
    CHECK(origin->backend == replacement);
    CHECK_FALSE(pending.consume(*second));
}

TEST_CASE("a 26.2 backend and client share ids that renumber to the session numbering and back") {
    namespace multiversion = basalt::wire::multiversion;
    constexpr auto kPlay = basalt::wire::protocol::ProtocolState::Play;
    IdRenumbering ids{multiversion::java::javaTranslators::chainFor(multiversion::protocolVersions::kV26_2)};
    const std::array<std::uint8_t, 4> keepAlive{0x2c, 0, 0, 7};
    io::ByteBuffer session;
    io::ByteBuffer back;
    REQUIRE(ids.apply(IdRenumbering::Flow::kBackendToSession, kPlay, keepAlive, session) ==
            IdRenumbering::Outcome::kRenumbered);
    CHECK(session.readable()[0] == 0x2d);
    REQUIRE(ids.apply(IdRenumbering::Flow::kSessionToClient, kPlay, session.readable(), back) ==
            IdRenumbering::Outcome::kRenumbered);
    CHECK(std::ranges::equal(back.readable(), keepAlive));
    CHECK(resolveRoute(multiversion::protocolVersions::kV26_2, multiversion::protocolVersions::kV26_2));
    CHECK_FALSE(resolveRoute(multiversion::protocolVersions::kV26_1, multiversion::protocolVersions::kV26_2));
}
