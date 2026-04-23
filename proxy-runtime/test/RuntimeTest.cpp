#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "RuntimeFixture.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/auth/Cfb8Cipher.hpp>
#include <basalt/wire/io/EventLoop.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

#include <doctest.h>

#include <RsaClient.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace basalt::proxy;
namespace io = basalt::wire::io;
using namespace std::chrono_literals;

using namespace basalt::proxy::test;

TEST_CASE("public listener handles fragmented handshake and pipelined status/pong independently of Wire encoders") {
    Running server;
    auto client = server.connect();
    const auto hello = handshake(server.runtime.port());
    REQUIRE(client.writeSome(std::span{hello}.first(2)));
    REQUIRE(client.writeSome(std::span{hello}.subspan(2)));
    const std::array<std::uint8_t, 12> request{1, 0, 9, 1, 1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(client.writeSome(request));
    const auto response = receive(client, 1, true);
    std::size_t offset = 0;
    const auto frameLength = integer(response, offset);
    const auto frameStart = offset;
    REQUIRE(integer(response, offset) == 0);
    const auto jsonLength = integer(response, offset);
    REQUIRE(offset + jsonLength <= response.size());
    const std::string json(response.begin() + static_cast<std::ptrdiff_t>(offset),
                           response.begin() + static_cast<std::ptrdiff_t>(offset + jsonLength));
    CHECK(json.find("\"protocol\":777") != std::string::npos);
    CHECK(json.find("\"online\":0") != std::string::npos);
    offset += jsonLength;
    CHECK(offset == frameStart + frameLength);
    REQUIRE(response.size() - offset == 10);
    const std::array<std::uint8_t, 10> pong{9, 1, 1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(std::equal(response.begin() + static_cast<std::ptrdiff_t>(offset), response.end(), pong.begin()));
    server.stop();
    CHECK(server.runtime.sessionCount() == 0);
    CHECK(server.runtime.bufferedBytes() == 0);
}

TEST_CASE("partial handshake deadline is absolute and a silent peer does not block other owners") {
    auto config = settings();
    config.runtime.handshakeTimeoutMs = 120;
    Running server{config};
    const auto start = std::chrono::steady_clock::now();
    auto slow = server.connect();
    const std::array<std::uint8_t, 1> fragment{0x80};
    REQUIRE(slow.writeSome(fragment));
    auto healthy = server.connect();
    auto hello = handshake(server.runtime.port());
    REQUIRE(healthy.writeSome(hello));
    const std::array<std::uint8_t, 2> request{1, 0};
    REQUIRE(healthy.writeSome(request));
    CHECK_FALSE(receive(healthy, 1).empty());
    CHECK(receive(slow, 0, true).empty());
    CHECK(std::chrono::steady_clock::now() - start >= 100ms);
    CHECK(std::chrono::steady_clock::now() - start < 1s);
}

TEST_CASE("bootstrap framing is bounded before reading a declared body") {
    Running server;
    for (const auto& packet : {std::vector<std::uint8_t>{0}, std::vector<std::uint8_t>{0x80, 0x80, 0x80},
                               std::vector<std::uint8_t>{0xff, 0x7f}, std::vector<std::uint8_t>{1, 3}}) {
        auto client = server.connect();
        REQUIRE(client.writeSome(packet));
        CHECK(receive(client, 0, true).empty());
    }
    auto forged = server.connect();
    auto carrier = handshake(server.runtime.port());
    carrier[5] = 0;
    REQUIRE(forged.writeSome(carrier));
    CHECK(receive(forged, 0, true).empty());
}

TEST_CASE("global session capacity is enforced and stop drains queued and owned sockets") {
    auto config = settings();
    config.runtime.maxSessions = 2;
    Running server{config};
    auto first = server.connect();
    auto second = server.connect();
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (server.runtime.sessionCount() != 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    REQUIRE(server.runtime.sessionCount() == 2);
    auto denied = server.connect();
    CHECK(receive(denied, 0, true).empty());
    CHECK(server.runtime.bufferedBytes() <= config.runtime.maxBufferedBytes);
    server.stop();
    CHECK(server.runtime.sessionCount() == 0);
    CHECK(server.runtime.bufferedBytes() == 0);
    CHECK(receive(first, 0, true).empty());
    CHECK(receive(second, 0, true).empty());
}

TEST_CASE("runtime shutdown is finite during handoffs, partial frames and pending status") {
    Running server;
    std::vector<io::TcpConnection> clients;
    for (int i = 0; i < 48; ++i) {
        clients.push_back(server.connect());
        if (i % 2 == 0) {
            const auto hello = handshake(server.runtime.port());
            REQUIRE(clients.back().writeSome(hello));
            const std::array<std::uint8_t, 2> request{1, 0};
            REQUIRE(clients.back().writeSome(request));
        }
    }
    const auto begin = std::chrono::steady_clock::now();
    server.stop();
    CHECK(std::chrono::steady_clock::now() - begin < 1s);
    CHECK(server.runtime.sessionCount() == 0);
    CHECK(server.runtime.bufferedBytes() == 0);
}

TEST_CASE("online key exchange runs session lookup on a worker and encrypts subsequent rejection") {
    using namespace basalt::wire::auth;
    std::atomic<bool> fetched{false};
    runtime::RuntimeServices services{[&](auto url, auto deadline, auto stop) {
        CHECK(url.find("username=Tester&serverId=") != std::string::npos);
        CHECK(std::chrono::steady_clock::now() < deadline);
        CHECK_FALSE(stop.stop_requested());
        fetched.store(true);
        return std::expected<HttpsResponse, HttpsError>{fakeProfile()};
    }};
    auto config = settings();
    bool transfer = false;
    SUBCASE("normal login") {}
    SUBCASE("accepted transfer still performs Mojang authentication") {
        config.network.acceptTransfers = true;
        transfer = true;
    }
    Running server{config, std::move(services)};
    auto client = server.connect();
    loginStart(client, server.runtime.port(), transfer ? 3 : 2);
    const auto secret = keyResponse(client);
    auto response = receive(client, 1, true);
    auto decrypt = Cfb8Cipher::create(secret);
    REQUIRE(decrypt);
    decrypt->decrypt(response);
    CHECK(fetched.load());
    CHECK(std::string(response.begin(), response.end()).find("No server is available right now") != std::string::npos);
}

TEST_CASE("reload swaps backends and routes together and refuses fixed settings") {
    auto config = settings();
    config.backends = {{.name = "a", .host = "127.0.0.1", .port = 1, .forwarding = {}}};
    config.defaultRoute = {"a"};
    Running proxy{config};
    CHECK(proxy.runtime.backendNames() == std::vector<std::string>{"a"});

    auto fresh = config;
    fresh.backends.push_back({.name = "b", .host = "127.0.0.1", .port = 2, .forwarding = {}});
    REQUIRE(proxy.runtime.reload(fresh) == 2);
    CHECK(proxy.runtime.backendNames() == std::vector<std::string>{"a", "b"});

    auto broken = fresh;
    broken.defaultRoute = {"missing"};
    CHECK_FALSE(proxy.runtime.reload(broken));
    auto moved = fresh;
    moved.network.compressionThreshold = 1;
    CHECK_FALSE(proxy.runtime.reload(moved));
    CHECK(proxy.runtime.backendNames() == std::vector<std::string>{"a", "b"});
}
