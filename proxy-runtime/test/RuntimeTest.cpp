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

TEST_CASE("offline login validates the name before rejecting unavailable routing") {
    auto config = settings();
    config.network.onlineMode = false;
    Running server{config};
    auto client = server.connect();
    const auto hello = handshake(server.runtime.port(), 2);
    REQUIRE(client.writeSome(hello));
    std::array<std::uint8_t, 25> login{};
    login[0] = 24;
    login[1] = 0;
    login[2] = 6;
    const std::string name = "Tester";
    std::copy(name.begin(), name.end(), login.begin() + 3);
    REQUIRE(client.writeSome(login));
    const auto response = receive(client, 1, true);
    const std::string text(response.begin(), response.end());
    CHECK(text.find("Player routing is unavailable") != std::string::npos);
}

TEST_CASE("public accepts obey both the origin gate and the global retained buffer budget") {
    SUBCASE("origin gate") {
        auto config = settings();
        config.admission.originRate = 1;
        config.admission.originBurst = 1;
        Running server{config};
        auto admitted = server.connect();
        const auto deadline = std::chrono::steady_clock::now() + 500ms;
        while (server.runtime.sessionCount() != 1 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        REQUIRE(server.runtime.sessionCount() == 1);
        auto denied = server.connect();
        CHECK(receive(denied, 0, true).empty());
        CHECK(server.runtime.sessionCount() == 1);
    }
    SUBCASE("global buffer budget") {
        auto config = settings();
        config.runtime.maxBufferedBytes = 4096;
        config.runtime.sessionBufferBytes = 4096;
        config.runtime.highWaterBytes = 2048;
        config.runtime.lowWaterBytes = 1024;
        Running server{config};
        auto admitted = server.connect();
        const auto deadline = std::chrono::steady_clock::now() + 500ms;
        while (server.runtime.sessionCount() != 1 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        REQUIRE(server.runtime.sessionCount() == 1);
        auto denied = server.connect();
        CHECK(receive(denied, 0, true).empty());
        CHECK(server.runtime.sessionCount() == 1);
        CHECK(server.runtime.bufferedBytes() <= config.runtime.maxBufferedBytes);
    }
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

TEST_CASE("IPv6 loopback uses the same owned status transport") {
    auto config = settings();
    config.network.bind = "::1";
    Running server{config};
    auto client = server.connect("::1");
    const auto hello = handshake(server.runtime.port());
    REQUIRE(client.writeSome(hello));
    const std::array<std::uint8_t, 2> request{1, 0};
    REQUIRE(client.writeSome(request));
    CHECK_FALSE(receive(client, 1).empty());
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
    Running server{settings(), std::move(services)};
    auto client = server.connect();
    loginStart(client, server.runtime.port());
    const auto secret = keyResponse(client);
    auto response = receive(client, 1, true);
    auto decrypt = Cfb8Cipher::create(secret);
    REQUIRE(decrypt);
    decrypt->decrypt(response);
    CHECK(fetched.load());
    CHECK(std::string(response.begin(), response.end()).find("Player routing is unavailable") != std::string::npos);
}

TEST_CASE("forged challenge never reaches the session service") {
    using namespace basalt::wire::auth;
    std::atomic<int> calls{0};
    Running server{settings(), {[&](auto, auto, auto) {
                       calls.fetch_add(1);
                       return std::expected<HttpsResponse, HttpsError>{fakeProfile()};
                   }}};
    auto client = server.connect();
    loginStart(client, server.runtime.port());
    (void)keyResponse(client, true);
    CHECK(receive(client, 0, true).empty());
    CHECK(calls.load() == 0);
}

TEST_CASE("closing during authentication cancels the worker and drops stale results") {
    using namespace basalt::wire::auth;
    std::promise<void> entered;
    auto waiting = entered.get_future();
    std::atomic<bool> canceled{false};
    Running server{settings(), {[&](auto, auto deadline, std::stop_token stop) {
                       entered.set_value();
                       std::mutex mutex;
                       std::condition_variable_any condition;
                       std::unique_lock lock{mutex};
                       condition.wait_until(lock, stop, deadline, [] { return false; });
                       canceled.store(stop.stop_requested());
                       return std::expected<HttpsResponse, HttpsError>{
                           std::unexpected(HttpsError{"fixture canceled", HttpsError::Kind::Canceled})};
                   }}};
    auto client = server.connect();
    loginStart(client, server.runtime.port());
    (void)keyResponse(client);
    REQUIRE(waiting.wait_for(1s) == std::future_status::ready);
    client.close();
    const auto canceledDeadline = std::chrono::steady_clock::now() + 1s;
    while (!canceled.load() && std::chrono::steady_clock::now() < canceledDeadline)
        std::this_thread::sleep_for(1ms);
    CHECK(canceled.load());
    auto healthy = server.connect();
    const auto hello = handshake(server.runtime.port());
    REQUIRE(healthy.writeSome(hello));
    const std::array<std::uint8_t, 2> request{1, 0};
    REQUIRE(healthy.writeSome(request));
    CHECK_FALSE(receive(healthy, 1).empty());
    server.stop();
    CHECK(canceled.load());
    CHECK(server.runtime.sessionCount() == 0);
    CHECK(server.runtime.bufferedBytes() == 0);
}

TEST_CASE("a rejected online identity receives an encrypted failure rather than an offline fallback") {
    using namespace basalt::wire::auth;
    auto config = settings();
    config.backends = {{.name = "protected", .host = "protected.test"}};
    config.defaultRoute = {"protected"};
    std::atomic<unsigned> resolutions{0};
    runtime::RuntimeServices services;
    services.authentication = [](auto, auto, auto) {
        return std::expected<HttpsResponse, HttpsError>{HttpsResponse{204, {}}};
    };
    services.resolution = [&](auto, auto, const auto&) -> std::expected<std::vector<io::TcpAddress>, io::DnsError> {
        resolutions.fetch_add(1);
        return std::unexpected(io::DnsError{io::DnsError::Kind::ResolveFailed});
    };
    Running server{std::move(config), std::move(services)};
    auto client = server.connect();
    loginStart(client, server.runtime.port());
    const auto secret = keyResponse(client);
    auto response = receive(client, 1, true);
    auto decrypt = Cfb8Cipher::create(secret);
    REQUIRE(decrypt);
    decrypt->decrypt(response);
    const std::string text(response.begin(), response.end());
    CHECK(text.find("Login could not be completed") != std::string::npos);
    CHECK(text.find("Player routing is unavailable") == std::string::npos);
    CHECK(resolutions.load() == 0);
}

TEST_CASE("verified offline login resolves ordered routes and connects the first available backend") {
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto live = io::TcpListener::bind(*address);
    auto closed = io::TcpListener::bind(*address);
    REQUIRE(live);
    REQUIRE(closed);
    REQUIRE(live->setNonBlocking());
    const auto livePort = live->localPort();
    const auto closedPort = closed->localPort();
    REQUIRE(livePort);
    REQUIRE(closedPort);
    closed->close();
    auto config = settings();
    config.network.onlineMode = false;
    config.backends = {{.name = "missing", .host = "missing.test", .port = *livePort},
                       {.name = "refused", .host = "refused.test", .port = *closedPort},
                       {.name = "live", .host = "live.test", .port = *livePort}};
    config.defaultRoute = {"missing"};
    config.forcedHosts = {{"LOCALHOST.", {"missing", "refused", "live"}}};
    std::vector<std::string> queries;
    runtime::RuntimeServices services;
    services.resolution = [&](auto host, auto port,
                              const auto& options) -> std::expected<std::vector<io::TcpAddress>, io::DnsError> {
        REQUIRE_FALSE(options.stop.stop_requested());
        REQUIRE(options.deadline > std::chrono::steady_clock::now());
        queries.emplace_back(host);
        if (host == "missing.test")
            return std::unexpected(io::DnsError{io::DnsError::Kind::ResolveFailed});
        return std::vector{*io::TcpAddress::literal("127.0.0.1", port)};
    };
    Running proxy{std::move(config), std::move(services)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    io::EventLoop loop;
    REQUIRE(loop.watch(live->nativeHandle(), {true, false}, 1));
    const auto event = loop.wait(2000);
    REQUIRE(event);
    REQUIRE_FALSE(event->empty());
    auto backend = live->accept();
    REQUIRE(backend);
    const auto expectedResponse = rejectBackendLogin(*backend, proxy.runtime.port());
    const auto response = receive(client, 1, true);
    CHECK(response == expectedResponse);
    proxy.stop();
    CHECK(queries == std::vector<std::string>{"missing.test", "refused.test", "live.test"});
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("closing a frontend cancels backend DNS without blocking another session") {
    auto config = settings();
    config.network.onlineMode = false;
    config.backends = {{.name = "slow", .host = "slow.test"}};
    config.defaultRoute = {"slow"};
    std::promise<void> entered;
    auto entering = entered.get_future();
    std::promise<void> canceled;
    auto canceling = canceled.get_future();
    runtime::RuntimeServices services;
    services.resolution = [&](auto, auto,
                              const auto& options) -> std::expected<std::vector<io::TcpAddress>, io::DnsError> {
        entered.set_value();
        std::mutex mutex;
        std::condition_variable_any condition;
        std::unique_lock lock{mutex};
        condition.wait_until(lock, options.stop, options.deadline, [] { return false; });
        if (options.stop.stop_requested())
            canceled.set_value();
        return std::unexpected(io::DnsError{io::DnsError::Kind::Canceled});
    };
    Running proxy{std::move(config), std::move(services)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    REQUIRE(entering.wait_for(2s) == std::future_status::ready);
    auto healthy = proxy.connect();
    REQUIRE(healthy.write(handshake(proxy.runtime.port())));
    const std::array<std::uint8_t, 2> request{1, 0};
    REQUIRE(healthy.write(request));
    CHECK_FALSE(receive(healthy, 1).empty());
    client.close();
    CHECK(canceling.wait_for(2s) == std::future_status::ready);
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("backend connection uses the native resolver with a numeric loopback address") {
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    auto config = settings();
    config.network.onlineMode = false;
    config.backends = {{.name = "loopback", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"loopback"};
    Running proxy{std::move(config)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    io::EventLoop loop;
    REQUIRE(loop.watch(listener->nativeHandle(), {true, false}, 1));
    auto events = loop.wait(2000);
    REQUIRE(events);
    REQUIRE_FALSE(events->empty());
    auto backend = listener->accept();
    REQUIRE(backend);
    const auto expectedResponse = rejectBackendLogin(*backend, proxy.runtime.port());
    CHECK(receive(client, 1, true) == expectedResponse);
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("online identity is sent to a native backend and its disconnect is encrypted for the frontend") {
    using namespace basalt::wire::auth;
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    const auto port = listener->localPort();
    REQUIRE(port);
    auto config = settings();
    config.backends = {{.name = "native", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"native"};
    runtime::RuntimeServices services;
    services.authentication = [](auto, auto, auto) { return std::expected<HttpsResponse, HttpsError>{fakeProfile()}; };
    Running proxy{std::move(config), std::move(services)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    io::EventLoop loop;
    REQUIRE(loop.watch(listener->nativeHandle(), {true, false}, 1));
    auto premature = loop.wait(0);
    REQUIRE(premature);
    CHECK(premature->empty());
    const auto secret = keyResponse(client);
    const auto connected = loop.wait(2000);
    REQUIRE(connected);
    REQUIRE_FALSE(connected->empty());
    auto backend = listener->accept();
    REQUIRE(backend);
    const auto expectedResponse = rejectBackendLogin(*backend, proxy.runtime.port());
    auto encrypted = receive(client, 1, true);
    auto decryptor = Cfb8Cipher::create(secret);
    REQUIRE(decryptor);
    decryptor->decrypt(encrypted);
    CHECK(encrypted == expectedResponse);
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("backend login rejects oversized frames and malformed disconnects without frontend acceptance") {
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    auto config = settings();
    config.network.onlineMode = false;
    config.backends = {{.name = "invalid", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"invalid"};
    Running proxy{std::move(config)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    io::EventLoop loop;
    REQUIRE(loop.watch(listener->nativeHandle(), {true, false}, 1));
    auto events = loop.wait(2000);
    REQUIRE(events);
    REQUIRE_FALSE(events->empty());
    auto backend = listener->accept();
    REQUIRE(backend);
    REQUIRE(backend->setNonBlocking());
    REQUIRE(receive(*backend, 42).size() == 42);
    std::vector<std::uint8_t> invalid;
    SUBCASE("declared body exceeds bootstrap quota before any body arrives") { invalid = {0xff, 0x7f}; }
    SUBCASE("invalid JSON cannot become a disconnect component") { invalid = {3, 0, 1, '{'}; }
    SUBCASE("a disconnect cannot hide another field") { invalid = {5, 0, 2, '{', '}', 1}; }
    REQUIRE(backend->write(invalid));
    CHECK(receive(client, 0, true).empty());
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("a silent backend cannot extend the absolute login deadline") {
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    auto config = settings();
    config.network.onlineMode = false;
    config.runtime.loginTimeoutMs = 200;
    config.backends = {{.name = "silent", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"silent"};
    Running proxy{std::move(config)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    io::EventLoop loop;
    REQUIRE(loop.watch(listener->nativeHandle(), {true, false}, 1));
    auto ready = loop.wait(1000);
    REQUIRE(ready);
    REQUIRE_FALSE(ready->empty());
    auto backend = listener->accept();
    REQUIRE(backend);
    REQUIRE(backend->setNonBlocking());
    REQUIRE(receive(*backend, 42).size() == 42);
    CHECK(receive(client, 0, true).empty());
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("backend compression is decoded independently of uncompressed frontend framing") {
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    const auto port = listener->localPort();
    REQUIRE(port);
    auto config = settings();
    config.network.onlineMode = false;
    config.backends = {{.name = "compressed", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"compressed"};
    Running proxy{std::move(config)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    io::EventLoop loop;
    REQUIRE(loop.watch(listener->nativeHandle(), {true, false}, 1));
    auto events = loop.wait(2000);
    REQUIRE(events);
    REQUIRE_FALSE(events->empty());
    auto backend = listener->accept();
    REQUIRE(backend);
    REQUIRE(backend->setNonBlocking());
    REQUIRE(receive(*backend, 42).size() == 42);
    const std::string reason = "{\"text\":\"Compressed denial\"}";
    std::vector<std::uint8_t> expected{static_cast<std::uint8_t>(reason.size() + 2), 0,
                                       static_cast<std::uint8_t>(reason.size())};
    expected.insert(expected.end(), reason.begin(), reason.end());
    std::vector<std::uint8_t> packets;
    bool rejected = false;
    SUBCASE("coalesced threshold zero and zlib packet") {
        // Generated independently with Python zlib.compress on packet id/string payload.
        packets = {2,  3,  0,  39, 30, 120, 156, 99, 144, 169, 86,  42, 73,  173, 40,  81, 178, 82,  114, 206, 207, 45,
                   40, 74, 45, 46, 78, 77,  81,  72, 73,  205, 203, 76, 204, 81,  170, 5,  0,   147, 218, 10,  62};
    }
    SUBCASE("below threshold body uses the zero data-length carrier") {
        packets = {3, 3, 0x80, 2, static_cast<std::uint8_t>(expected.size()), 0};
        packets.insert(packets.end(), expected.begin() + 1, expected.end());
    }
    SUBCASE("negative threshold keeps backend framing uncompressed") {
        packets = {6, 3, 255, 255, 255, 255, 15};
        packets.insert(packets.end(), expected.begin(), expected.end());
    }
    SUBCASE("expanded size is rejected before inflation can allocate") {
        packets = {2, 3, 0, 3, 0x81, 0x08, 0};
        rejected = true;
    }
    SUBCASE("truncated compression header cannot borrow the next frame") {
        packets = {2, 3, 0, 1, 0x80, 1, 0};
        rejected = true;
    }
    SUBCASE("compression control cannot be negotiated twice") {
        packets = {2, 3, 0, 3, 0, 3, 0};
        rejected = true;
    }
    REQUIRE(backend->write(packets));
    const auto response = receive(client, 0, true);
    if (rejected)
        CHECK(response.empty());
    else
        CHECK(response == expected);
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}
