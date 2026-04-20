#pragma once

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

namespace basalt::proxy::test {
namespace io = basalt::wire::io;
using namespace std::chrono_literals;

inline config::ProxyConfig settings() {
    config::ProxyConfig result;
    result.network.bind = "127.0.0.1";
    result.network.port = 0;
    result.network.compressionThreshold = -1;
    result.runtime.ioLoops = 3;
    result.runtime.handshakeTimeoutMs = 1000;
    result.admission.originBurst = 100;
    return result;
}

inline runtime::ProxyRuntime makeRuntime(config::ProxyConfig config, runtime::RuntimeServices services = {}) {
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

inline std::vector<std::uint8_t> receive(io::TcpConnection& client, std::size_t minimum, bool expectEof = false) {
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

inline std::array<std::uint8_t, 17> handshake(std::uint16_t port, std::uint8_t intent = 1) {
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

inline std::uint32_t integer(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
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

inline void loginStart(io::TcpConnection& client, std::uint16_t port, std::uint8_t intent = 2) {
    const auto hello = handshake(port, intent);
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

inline basalt::wire::auth::LoginSecret keyResponse(io::TcpConnection& client, bool wrongNonce = false) {
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

inline basalt::wire::auth::HttpsResponse fakeProfile() {
    const auto id = basalt::wire::common::Uuid::offlinePlayer("Tester").toUndashedString();
    const std::string json = "{\"id\":\"" + id + "\",\"name\":\"Tester\"}";
    return {200, {json.begin(), json.end()}};
}

inline std::vector<std::uint8_t> rejectBackendLogin(io::TcpConnection& backend, std::uint16_t virtualPort) {
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
} // namespace basalt::proxy::test
