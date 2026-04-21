#include "../src/BackendLogin.hpp"
#include "RuntimeFixture.hpp"

#include <basalt/wire/auth/forwarding/VelocityForwarding.hpp>
#include <basalt/wire/io/BudgetMemoryResource.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>

using namespace basalt::proxy;
using namespace basalt::proxy::test;
using runtime::detail::BackendLogin;
namespace protocol = basalt::wire::protocol;
namespace forwarding = basalt::wire::auth::forwarding;

TEST_CASE("two live backend transports keep play and Velocity login configuration independent") {
    const auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    const auto port = listener->localPort();
    REQUIRE(port);
    std::array<io::TcpConnection, 2> connections;
    std::array<io::TcpConnection, 2> peers;
    for (std::size_t i = 0; i < connections.size(); ++i) {
        auto connected = io::TcpConnection::connect("127.0.0.1", *port);
        REQUIRE(connected);
        REQUIRE(connected->setNonBlocking());
        auto accepted = listener->accept();
        REQUIRE(accepted);
        REQUIRE(accepted->setNonBlocking());
        connections[i] = std::move(*connected);
        peers[i] = std::move(*accepted);
    }
    io::ResourceBudget global{512 * 1024};
    io::ResourceBudget local{512 * 1024};
    io::BudgetMemoryResource memory{global, local};
    {
        config::BackendConfig plain;
        config::BackendConfig modern;
        modern.forwarding = forwarding::ForwardingMode::Velocity;
        modern.secret = "parallel-candidate-secret";
        basalt::wire::auth::SessionProfile profile;
        profile.name = "Tester";
        profile.id = basalt::wire::common::Uuid::offlinePlayer(profile.name);
        BackendLogin active{std::move(connections[0]), memory, {1, 0}};
        BackendLogin candidate{std::move(connections[1]), memory, {2, 1}};
        REQUIRE(active.start(plain, profile, "localhost", 25565, "127.0.0.1"));
        REQUIRE(candidate.start(modern, profile, "localhost", 25565, "127.0.0.1"));
        std::array endpoints{&active, &candidate};
        std::array<std::vector<std::uint8_t>, 2> packets;
        io::EventLoop loop;
        const auto pump = [&](const auto& done) {
            const auto limit = BackendLogin::Clock::now() + 2s;
            while (!done() && BackendLogin::Clock::now() < limit) {
                for (std::size_t i = 0; i < endpoints.size(); ++i)
                    REQUIRE(loop.watch(endpoints[i]->handle(), endpoints[i]->interest(), i + 1));
                const auto ready = loop.wait(50);
                REQUIRE(ready);
                for (const auto& event : *ready) {
                    REQUIRE(event.token >= 1);
                    REQUIRE(event.token <= endpoints.size());
                    const auto index = static_cast<std::size_t>(event.token - 1);
                    auto packet = endpoints[index]->ready(event);
                    REQUIRE(packet);
                    if (!packet->empty()) {
                        REQUIRE(packets[index].empty());
                        packets[index].assign(packet->begin(), packet->end());
                    }
                }
            }
            REQUIRE(done());
        };
        const auto drain = [&] { pump([&] { return !active.hasOutput() && !candidate.hasOutput(); }); };
        const auto body = [&](std::size_t index, std::int32_t expectedId) {
            io::ByteBuffer result{packets[index].size()};
            result.writeBytes(packets[index]);
            REQUIRE(protocol::varInt::read(result) == expectedId);
            packets[index].clear();
            return result;
        };
        drain();
        CHECK(receive(peers[0], 42).size() == 42);
        CHECK(receive(peers[1], 42).size() == 42);
        std::vector<std::uint8_t> finished{41,   2,    0xf3, 0xd2, 0x8c, 0xb0, 0x72, 0x25, 0x3c, 0xb1, 0xba, 0xeb, 0x2d,
                                           0xad, 0xd2, 0xbe, 0x89, 0xae, 6,    'T',  'e',  's',  't',  'e',  'r',  0};
        finished.insert(finished.end(), 16, 0);
        REQUIRE(peers[0].write(finished));
        pump([&] { return !packets[0].empty(); });
        auto activeLogin = body(0, 2);
        REQUIRE(active.acceptLogin(activeLogin));
        REQUIRE(active.queue(std::array<std::uint8_t, 1>{3}));
        REQUIRE(active.acknowledgeLogin());
        drain();
        CHECK(receive(peers[0], 2) == std::vector<std::uint8_t>{1, 3});
        REQUIRE(peers[0].write(std::array<std::uint8_t, 2>{1, 3}));
        pump([&] { return !packets[0].empty(); });
        auto finish = body(0, 3);
        REQUIRE(finish.readableBytes() == 0);
        REQUIRE(active.queue(std::array<std::uint8_t, 1>{3}));
        REQUIRE(active.finishConfiguration());
        drain();
        CHECK(receive(peers[0], 2) == std::vector<std::uint8_t>{1, 3});
        std::vector<std::uint8_t> query{24, 4, 42, 20};
        constexpr std::string_view channel = "velocity:player_info";
        query.insert(query.end(), channel.begin(), channel.end());
        query.push_back(4);
        const std::vector<std::uint8_t> keepalive{9, 0x2d, 0, 0, 0, 0, 0, 0, 0, 99};
        REQUIRE(peers[0].write(keepalive));
        REQUIRE(peers[1].write(query));
        pump([&] { return !packets[0].empty() && !packets[1].empty(); });
        CHECK(packets[0] == std::vector<std::uint8_t>{keepalive.begin() + 1, keepalive.end()});
        auto reply = packets[0];
        reply[0] = 0x1c;
        packets[0].clear();
        REQUIRE(active.queue(reply));
        auto loginQuery = body(1, 4);
        REQUIRE(candidate.answerPluginQuery(loginQuery));
        drain();
        auto expectedKeepalive = keepalive;
        expectedKeepalive[1] = 0x1c;
        CHECK(receive(peers[0], expectedKeepalive.size()) == expectedKeepalive);
        auto forwarded = receive(peers[1], 71);
        io::ByteBuffer response{forwarded.size()};
        response.writeBytes(forwarded);
        REQUIRE(protocol::varInt::read(response) == 70);
        REQUIRE(protocol::varInt::read(response) == 2);
        REQUIRE(protocol::varInt::read(response) == 42);
        REQUIRE(protocol::readBoolean(response));
        forwarding::ProxyForwarding policy{.mode = forwarding::ForwardingMode::Velocity, .secret = modern.secret};
        const auto decoded = forwarding::velocity::decode(response.readable(), policy);
        REQUIRE(forwarding::accepted(decoded));
        CHECK(std::get<forwarding::ForwardedLogin>(decoded).profileId == profile.id);
        CHECK(std::get<forwarding::ForwardedLogin>(decoded).name == profile.name);
        policy.secret = "unrelated-secret";
        CHECK_FALSE(forwarding::accepted(forwarding::velocity::decode(response.readable(), policy)));
        REQUIRE(peers[1].write(finished));
        pump([&] { return !packets[1].empty(); });
        auto candidateLogin = body(1, 2);
        REQUIRE(candidate.acceptLogin(candidateLogin));
        REQUIRE(candidate.queue(std::array<std::uint8_t, 1>{3}));
        REQUIRE(candidate.acknowledgeLogin());
        drain();
        CHECK(receive(peers[1], 2) == std::vector<std::uint8_t>{1, 3});
        REQUIRE(peers[1].write(std::array<std::uint8_t, 3>{2, 15, 0}));
        REQUIRE(peers[0].write(keepalive));
        pump([&] { return !packets[0].empty() && !packets[1].empty(); });
        CHECK(packets[1] == std::vector<std::uint8_t>{15, 0});
        CHECK(packets[0] == std::vector<std::uint8_t>{keepalive.begin() + 1, keepalive.end()});
        CHECK(active.phase() == BackendLogin::Phase::kPlay);
        CHECK(candidate.phase() == BackendLogin::Phase::kConfiguration);
        CHECK_FALSE(active.velocityForwarded());
        CHECK(candidate.velocityForwarded());
        packets[0].clear();
        packets[1].clear();
        constexpr std::string_view key = "fixture:key";
        REQUIRE_FALSE(candidate.relayCookie(
            {std::string{key}, std::vector<std::uint8_t>(basalt::wire::protocol::cookie::kMaxPayloadBytes + 1, 0)}));
        CHECK_FALSE(candidate.hasOutput());
        REQUIRE(active.relayCookie({std::string{key}, std::nullopt}));
        REQUIRE(candidate.relayCookie({std::string{key}, std::vector<std::uint8_t>{}}));
        drain();
        std::vector<std::uint8_t> playCookie{14, 0x15, 11};
        playCookie.insert(playCookie.end(), key.begin(), key.end());
        playCookie.push_back(0);
        std::vector<std::uint8_t> configurationCookie{15, 1, 11};
        configurationCookie.insert(configurationCookie.end(), key.begin(), key.end());
        configurationCookie.insert(configurationCookie.end(), {1, 0});
        CHECK(receive(peers[0], playCookie.size()) == playCookie);
        CHECK(receive(peers[1], configurationCookie.size()) == configurationCookie);
        CHECK(local.used() >= active.allocated() + candidate.allocated());
    }
    CHECK(global.used() == 0);
    CHECK(local.used() == 0);
}
