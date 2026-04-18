#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/io/EventLoop.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

#include <doctest.h>

#include <array>
#include <chrono>
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

runtime::ProxyRuntime makeRuntime(config::ProxyConfig config) {
    auto created = runtime::ProxyRuntime::create(std::move(config));
    if (!created)
        throw std::runtime_error(created.error());
    return std::move(*created);
}

struct Running {
    runtime::ProxyRuntime runtime;
    std::optional<std::string> failure;
    std::jthread driver;

    explicit Running(config::ProxyConfig config = settings())
        : runtime(makeRuntime(std::move(config))), driver([this](std::stop_token stop) {
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

TEST_CASE("native login is rejected explicitly until routing is integrated") {
    Running server;
    auto client = server.connect();
    const auto hello = handshake(server.runtime.port(), 2);
    REQUIRE(client.writeSome(hello));
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
