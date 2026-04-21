#include "../src/BackendLogin.hpp"
#include "RuntimeFixture.hpp"

#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/io/BudgetMemoryResource.hpp>

using namespace basalt::proxy;
using namespace basalt::proxy::test;
using runtime::detail::BackendLogin;

TEST_CASE("backend deadlines are independent and write timeout resets only after output drains") {
    const auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    const auto port = listener->localPort();
    REQUIRE(port);
    auto connected = io::TcpConnection::connect("127.0.0.1", *port);
    REQUIRE(connected);
    REQUIRE(connected->setNonBlocking());
    auto peer = listener->accept();
    REQUIRE(peer);
    REQUIRE(peer->setNonBlocking());
    io::ResourceBudget global{64 * 1024};
    io::ResourceBudget local{64 * 1024};
    io::BudgetMemoryResource memory{global, local};
    {
        BackendLogin active{std::move(*connected), memory, {1, 0}};
        BackendLogin candidate{io::TcpConnection{}, memory, {2, 1}};
        basalt::wire::auth::SessionProfile profile;
        profile.name = "Tester";
        profile.id = basalt::wire::common::Uuid::offlinePlayer(profile.name);
        config::BackendConfig config;
        REQUIRE(active.start(config, profile, "localhost", 25565, "127.0.0.1"));
        REQUIRE(candidate.start(config, profile, "localhost", 25565, "127.0.0.1"));
        const auto base = BackendLogin::Clock::now() + 1h;
        active.expectReadUntil(base + 30ms);
        active.expectWriteUntil(base + 20ms);
        active.expectWriteUntil(base + 40ms);
        CHECK(active.deadline() == base + 20ms);
        CHECK(active.deadline(true) == base + 20ms);
        active.expectReadUntil(base + 10ms);
        CHECK(active.deadline() == base + 10ms);
        CHECK(active.deadline(true) == base + 20ms);
        candidate.expectWriteUntil(base + 5ms);
        candidate.expectReadUntil(base + 15ms);
        CHECK(candidate.deadline() == base + 5ms);
        CHECK(active.deadline() == base + 10ms);
        io::EventLoop loop;
        REQUIRE(loop.watch(active.handle(), {false, true}, 1));
        const auto limit = BackendLogin::Clock::now() + 2s;
        while (active.hasOutput() && BackendLogin::Clock::now() < limit) {
            const auto ready = loop.wait(100);
            REQUIRE(ready);
            for (const auto& event : *ready)
                REQUIRE(active.ready(event, false));
        }
        REQUIRE_FALSE(active.hasOutput());
        CHECK_FALSE(receive(*peer, 1).empty());
        CHECK(active.deadline() == base + 10ms);
        CHECK(active.deadline(true) == BackendLogin::Clock::time_point::max());
        active.expectWriteUntil(base + 1ms);
        CHECK(active.deadline(true) == BackendLogin::Clock::time_point::max());
        REQUIRE(active.queue(std::array<std::uint8_t, 1>{0}));
        active.expectWriteUntil(base + 50ms);
        CHECK(active.deadline(true) == base + 50ms);
        active.expectReadUntil(base + 60ms);
        CHECK(active.deadline() == base + 50ms);
        CHECK(candidate.deadline() == base + 5ms);
    }
    CHECK(local.used() == 0);
    CHECK(global.used() == 0);
}

TEST_CASE("configuration traffic cannot extend the absolute backend preparation deadline") {
    const auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    const auto port = listener->localPort();
    REQUIRE(port);
    auto config = settings();
    config.network.onlineMode = false;
    config.runtime.loginTimeoutMs = 1000;
    config.runtime.readTimeoutMs = 3000;
    config.backends = {{.name = "slow", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"slow"};
    Running proxy{std::move(config)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    io::EventLoop loop;
    REQUIRE(loop.watch(listener->nativeHandle(), {true, false}, 1));
    const auto accepted = loop.wait(2000);
    REQUIRE(accepted);
    REQUIRE_FALSE(accepted->empty());
    auto backend = listener->accept();
    REQUIRE(backend);
    REQUIRE(backend->setNonBlocking());
    REQUIRE(receive(*backend, 42).size() == 42);
    std::vector<std::uint8_t> finished{41,   2,    0xf3, 0xd2, 0x8c, 0xb0, 0x72, 0x25, 0x3c, 0xb1, 0xba, 0xeb, 0x2d,
                                       0xad, 0xd2, 0xbe, 0x89, 0xae, 6,    'T',  'e',  's',  't',  'e',  'r',  0};
    finished.insert(finished.end(), 16, 0);
    REQUIRE(backend->write(finished));
    REQUIRE(receive(client, finished.size()) == finished);
    const std::vector<std::uint8_t> ack{1, 3};
    REQUIRE(client.write(ack));
    REQUIRE(receive(*backend, ack.size()) == ack);
    loop.unwatch(listener->nativeHandle());
    REQUIRE(loop.watch(client.nativeHandle(), {true, false}, 2));
    const std::array<std::uint8_t, 10> keepalive{9, 4, 0, 0, 0, 0, 0, 0, 0, 1};
    const auto limit = BackendLogin::Clock::now() + 2s;
    bool eof = false;
    std::size_t traffic = 0;
    auto nextKeepalive = BackendLogin::Clock::now();
    while (!eof && BackendLogin::Clock::now() < limit) {
        if (BackendLogin::Clock::now() >= nextKeepalive) {
            (void)backend->write(keepalive);
            nextKeepalive = BackendLogin::Clock::now() + 50ms;
        }
        const auto ready = loop.wait(50);
        REQUIRE(ready);
        for (const auto& event : *ready) {
            if (!event.readable && !event.closed)
                continue;
            std::array<std::uint8_t, 2048> bytes{};
            const auto read = client.read(bytes);
            if (read) {
                traffic += *read;
                eof = *read == 0;
            } else {
                CHECK(read.error().kind == io::SocketError::Kind::WouldBlock);
            }
        }
    }
    CHECK(traffic >= keepalive.size());
    CHECK(eof);
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}
