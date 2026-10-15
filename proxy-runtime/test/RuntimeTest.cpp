#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
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

namespace {
config::ProxyConfig settings() {
    config::ProxyConfig result;
    result.network.bind = "127.0.0.1";
    result.network.port = 0;
    result.runtime.ioLoops = 3;
    result.runtime.handshakeTimeoutMs = 1000;
    result.admission.originBurst = 100;
    return result;
}

runtime::ProxyRuntime makeRuntime(config::ProxyConfig config, runtime::RuntimeServices services = {}) {
    auto created = runtime::ProxyRuntime::create(std::move(config), std::move(services));
    if (!created)
        throw std::runtime_error(created.error());
    return std::move(*created);
}

struct Running {
    runtime::ProxyRuntime runtime;
    std::optional<std::string> failure;
    std::jthread driver;

    explicit Running(config::ProxyConfig config = settings(), runtime::RuntimeServices services = {})
        : runtime(makeRuntime(std::move(config), std::move(services))), driver([this](std::stop_token stop) {
              const auto result = runtime.run(stop);
              if (!result)
                  failure = result.error();
          }) {}

    ~Running() { stop(); }

    void stop() {
        driver.request_stop();
        if (driver.joinable())
            driver.join();
        CHECK_FALSE(failure);
    }

    io::TcpConnection connect(std::string_view host = "127.0.0.1") {
        auto connected = io::TcpConnection::connect(host, runtime.port());
        if (!connected || !connected->setNonBlocking())
            throw std::runtime_error("fixture client connection failed");
        return std::move(*connected);
    }
};

std::vector<std::uint8_t> receive(io::TcpConnection& client, std::size_t minimum, bool expectEof = false) {
    io::EventLoop loop;
    if (!loop.watch(client.nativeHandle(), {true, false}, 1))
        throw std::runtime_error("fixture readiness failed");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::vector<std::uint8_t> output;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto ready = loop.wait(100);
        if (!ready)
            throw std::runtime_error("fixture readiness wait failed");
        std::array<std::uint8_t, 2048> bytes{};
        const auto read = client.read(bytes);
        if (!read) {
            if (read.error().kind == io::SocketError::Kind::WouldBlock)
                continue;
            throw std::runtime_error("fixture receive failed");
        }
        if (*read == 0)
            return output;
        output.insert(output.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(*read));
        if (!expectEof && output.size() >= minimum)
            return output;
    }
    throw std::runtime_error("fixture receive deadline expired");
}

std::array<std::uint8_t, 17> handshake(std::uint16_t port, std::uint8_t intent = 1) {
    return {16,
            0,
            0x89,
            0x06,
            9,
            'l',
            'o',
            'c',
            'a',
            'l',
            'h',
            'o',
            's',
            't',
            static_cast<std::uint8_t>(port >> 8),
            static_cast<std::uint8_t>(port),
            intent};
}

std::uint32_t integer(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 21; shift += 7) {
        if (offset >= bytes.size())
            throw std::runtime_error("fixture response ended inside a VarInt");
        const auto value = bytes[offset++];
        result |= static_cast<std::uint32_t>(value & 0x7fU) << shift;
        if ((value & 0x80U) == 0)
            return result;
    }
    throw std::runtime_error("fixture response had an oversized VarInt");
}

void loginStart(io::TcpConnection& client, std::uint16_t port) {
    const auto hello = handshake(port, 2);
    if (!client.writeSome(hello))
        throw std::runtime_error("fixture login handshake failed");
    std::array<std::uint8_t, 25> login{};
    login[0] = 24;
    login[2] = 6;
    const std::string name = "Tester";
    std::copy(name.begin(), name.end(), login.begin() + 3);
    if (!client.writeSome(login))
        throw std::runtime_error("fixture login hello failed");
}

basalt::wire::auth::LoginSecret keyResponse(io::TcpConnection& client, bool wrongNonce = false) {
    namespace auth = basalt::wire::auth;
    const auto hello = receive(client, 174);
    std::size_t offset = 0;
    const auto length = integer(hello, offset);
    const auto body = offset;
    if (integer(hello, offset) != 1 || integer(hello, offset) != 0)
        throw std::runtime_error("fixture encryption hello malformed");
    const auto keyBytes = integer(hello, offset);
    if (offset + keyBytes > hello.size())
        throw std::runtime_error("fixture public key incomplete");
    const std::span key{hello.data() + offset, keyBytes};
    offset += keyBytes;
    const auto nonceBytes = integer(hello, offset);
    if (nonceBytes != 4 || offset + nonceBytes + 1 != hello.size() || body + length != hello.size())
        throw std::runtime_error("fixture challenge malformed");
    std::array<std::uint8_t, 4> nonce{};
    std::copy_n(hello.begin() + static_cast<std::ptrdiff_t>(offset), 4, nonce.begin());
    if (hello.back() != 1)
        throw std::runtime_error("fixture proxy disabled authentication unexpectedly");
    if (wrongNonce)
        nonce[0] ^= 1;
    const auth::LoginSecret secret{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const auto encryptedSecret = auth::test::encryptRsa(key, secret);
    const auto encryptedNonce = auth::test::encryptRsa(key, nonce);
    if (encryptedSecret.size() != 128 || encryptedNonce.size() != 128)
        throw std::runtime_error("fixture ciphertext size changed");
    // Independent packet framing: length 261, packet 1, two length-128 ciphertexts.
    std::vector<std::uint8_t> packet{0x85, 0x02, 1, 0x80, 0x01};
    packet.insert(packet.end(), encryptedSecret.begin(), encryptedSecret.end());
    packet.insert(packet.end(), {0x80, 0x01});
    packet.insert(packet.end(), encryptedNonce.begin(), encryptedNonce.end());
    if (!client.writeSome(packet))
        throw std::runtime_error("fixture key response failed");
    return secret;
}

basalt::wire::auth::HttpsResponse fakeProfile() {
    const auto id = basalt::wire::common::Uuid::offlinePlayer("Tester").toUndashedString();
    const std::string json = "{\"id\":\"" + id + "\",\"name\":\"Tester\"}";
    return {200, {json.begin(), json.end()}};
}

std::vector<std::uint8_t> rejectBackendLogin(io::TcpConnection& backend, std::uint16_t virtualPort) {
    REQUIRE(backend.setNonBlocking());
    const auto bootstrap = receive(backend, 42);
    const auto expectedHandshake = handshake(virtualPort, 2);
    REQUIRE(bootstrap.size() == 42);
    CHECK(std::equal(expectedHandshake.begin(), expectedHandshake.end(), bootstrap.begin()));
    const std::array<std::uint8_t, 25> expectedLogin{24,   0,    6,    'T',  'e',  's',  't',  'e',  'r',
                                                     0xf3, 0xd2, 0x8c, 0xb0, 0x72, 0x25, 0x3c, 0xb1, 0xba,
                                                     0xeb, 0x2d, 0xad, 0xd2, 0xbe, 0x89, 0xae};
    CHECK(std::equal(expectedLogin.begin(), expectedLogin.end(), bootstrap.begin() + 17));
    const std::string reason = "{\"text\":\"Backend denied this login\"}";
    std::vector<std::uint8_t> disconnect{static_cast<std::uint8_t>(reason.size() + 2), 0,
                                         static_cast<std::uint8_t>(reason.size())};
    disconnect.insert(disconnect.end(), reason.begin(), reason.end());
    REQUIRE(backend.writeSome(std::span{disconnect}.first(1)));
    REQUIRE(backend.writeSome(std::span{disconnect}.subspan(1)));
    return disconnect;
}
} // namespace

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
