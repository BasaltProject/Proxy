#include "../src/IdentityRegistry.hpp"
#include "RuntimeFixture.hpp"

#include <barrier>

using namespace basalt::proxy;
using namespace basalt::proxy::test;
namespace io = basalt::wire::io;

TEST_CASE("UUID claims have a single winner across concurrent owners and can be released") {
    runtime::detail::IdentityRegistry identities{16};
    const auto id = basalt::wire::common::Uuid::offlinePlayer("Tester");
    std::barrier start{16};
    std::atomic<unsigned> winners{0};
    std::vector<std::jthread> contenders;
    for (int i = 0; i < 16; ++i)
        contenders.emplace_back([&] {
            start.arrive_and_wait();
            if (identities.claim(id))
                winners.fetch_add(1);
        });
    contenders.clear();
    CHECK(winners.load() == 1);
    identities.release(id);
    CHECK(identities.claim(id));
    identities.release(id);
}

TEST_CASE("a verified duplicate cannot displace the first session or start another backend login") {
    using namespace basalt::wire::auth;
    for (const bool online : {false, true}) {
        INFO("online = " << online);
        auto address = io::TcpAddress::literal("127.0.0.1", 0);
        REQUIRE(address);
        auto listener = io::TcpListener::bind(*address);
        REQUIRE(listener);
        REQUIRE(listener->setNonBlocking());
        const auto port = listener->localPort();
        REQUIRE(port);
        auto config = settings();
        config.network.onlineMode = online;
        config.backends = {{.name = "identity", .host = "fixture.test", .port = *port}};
        config.defaultRoute = {"identity"};
        std::atomic<unsigned> lookups{0};
        runtime::RuntimeServices services;
        services.authentication = [](auto, auto, auto) {
            return std::expected<HttpsResponse, HttpsError>{fakeProfile()};
        };
        services.resolution = [&](auto, auto destinationPort,
                                  const auto&) -> std::expected<std::vector<io::TcpAddress>, io::DnsError> {
            lookups.fetch_add(1);
            return std::vector{*io::TcpAddress::literal("127.0.0.1", destinationPort)};
        };
        Running proxy{std::move(config), std::move(services)};
        io::EventLoop loop;
        REQUIRE(loop.watch(listener->nativeHandle(), {true, false}, 1));
        const auto accept = [&]() {
            const auto events = loop.wait(2000);
            REQUIRE(events);
            REQUIRE_FALSE(events->empty());
            auto backend = listener->accept();
            REQUIRE(backend);
            REQUIRE(backend->setNonBlocking());
            return std::move(*backend);
        };
        auto first = proxy.connect();
        loginStart(first, proxy.runtime.port());
        std::optional<Cfb8Cipher> firstDecrypt;
        if (online) {
            const auto secret = keyResponse(first);
            firstDecrypt = Cfb8Cipher::create(secret);
            REQUIRE(firstDecrypt);
        }
        auto backend = accept();
        REQUIRE(receive(backend, 42).size() == 42);
        auto duplicate = proxy.connect();
        loginStart(duplicate, proxy.runtime.port());
        std::optional<Cfb8Cipher> duplicateDecrypt;
        if (online) {
            const auto secret = keyResponse(duplicate);
            duplicateDecrypt = Cfb8Cipher::create(secret);
            REQUIRE(duplicateDecrypt);
        }
        auto rejected = receive(duplicate, 1, true);
        if (duplicateDecrypt)
            duplicateDecrypt->decrypt(rejected);
        CHECK(std::string(rejected.begin(), rejected.end()).find("already connected") != std::string::npos);
        CHECK(lookups.load() == 1);
        REQUIRE(backend.write(std::array<std::uint8_t, 5>{4, 0, 2, '{', '}'}));
        auto response = receive(first, 1, true);
        if (firstDecrypt)
            firstDecrypt->decrypt(response);
        CHECK_FALSE(response.empty());
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (proxy.runtime.sessionCount() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        REQUIRE(proxy.runtime.sessionCount() == 0);
        auto reconnected = proxy.connect();
        loginStart(reconnected, proxy.runtime.port());
        if (online)
            (void)keyResponse(reconnected);
        auto replacement = accept();
        CHECK(receive(replacement, 42).size() == 42);
        CHECK(lookups.load() == 2);
        proxy.stop();
        CHECK(proxy.runtime.sessionCount() == 0);
        CHECK(proxy.runtime.bufferedBytes() == 0);
    }
}
