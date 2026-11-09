#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../src/BackendLogin.hpp"

#include "../src/NativeForwarding.hpp"
#include "RuntimeFixture.hpp"

#include <basalt/wire/auth/forwarding/BasaltForwarding.hpp>
#include <basalt/wire/auth/forwarding/VelocityForwarding.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/SessionEnvelope.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/play/ChatPackets.hpp>

#include <filesystem>
#include <fstream>

using namespace basalt::proxy;
using namespace basalt::proxy::test;
namespace io = basalt::wire::io;

TEST_CASE("unsupported login query receives a matching negative response with backend framing") {
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    auto config = settings();
    config.network.onlineMode = false;
    config.backends = {{.name = "query", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"query"};
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
    const std::string channel = "fixture:unknown";
    std::vector<std::uint8_t> query{20, 4, 42, 15};
    query.insert(query.end(), channel.begin(), channel.end());
    query.insert(query.end(), {0xab, 0xcd});
    std::vector<std::uint8_t> packets;
    std::vector<std::uint8_t> expected;
    SUBCASE("uncompressed backend") {
        packets = query;
        expected = {3, 2, 42, 0};
    }
    SUBCASE("signed transaction identifiers are returned unchanged") {
        packets = {24, 4, 255, 255, 255, 255, 15, 15};
        packets.insert(packets.end(), channel.begin(), channel.end());
        packets.insert(packets.end(), {0xab, 0xcd});
        expected = {7, 2, 255, 255, 255, 255, 15, 0};
    }
    SUBCASE("backend threshold above response size") {
        packets = {3, 3, 0x80, 2, 21, 0};
        packets.insert(packets.end(), query.begin() + 1, query.end());
        expected = {4, 0, 2, 42, 0};
    }
    SUBCASE("threshold zero compresses the response") {
        packets = {2, 3, 0};
        // Independent Python zlib vectors for query and response.
        const std::vector<std::uint8_t> compressedQuery{29, 20,  120, 156, 99,  209, 226, 79,  203, 172,
                                                        40, 41,  45,  74,  181, 42,  205, 203, 206, 203,
                                                        47, 207, 91,  125, 22,  0,   69,  172, 8,   7};
        packets.insert(packets.end(), compressedQuery.begin(), compressedQuery.end());
        expected = {12, 3, 120, 156, 99, 210, 98, 0, 0, 0, 93, 0, 45};
    }
    REQUIRE(backend->write(packets));
    CHECK(receive(*backend, expected.size()) == expected);
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("native login ACK and configuration finish gate bidirectional relay") {
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    auto config = settings();
    config.network.onlineMode = false;
    config.backends = {{.name = "native", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"native"};
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
    std::vector<std::uint8_t> finished{41,   2,    0xf3, 0xd2, 0x8c, 0xb0, 0x72, 0x25, 0x3c, 0xb1, 0xba, 0xeb, 0x2d,
                                       0xad, 0xd2, 0xbe, 0x89, 0xae, 6,    'T',  'e',  's',  't',  'e',  'r',  0};
    finished.insert(finished.end(), 15, 0);
    finished.push_back(1);
    bool loginEof = false;
    bool reconfigurationEof = false;
    bool wrongProfile = false;
    bool malformedAck = false;
    bool prematureFinishAck = false;
    bool configurationDisconnect = false;
    bool malformedStart = false;
    bool prematureReconfigurationAck = false;
    bool malformedReconfigurationAck = false;
    SUBCASE("valid native transition") {}
    SUBCASE("backend EOF while waiting for login ACK closes promptly") { loginEof = true; }
    SUBCASE("backend EOF while waiting for reconfiguration ACK closes promptly") { reconfigurationEof = true; }
    SUBCASE("start configuration cannot carry trailing fields") { malformedStart = true; }
    SUBCASE("play configuration acknowledgement requires a request") { prematureReconfigurationAck = true; }
    SUBCASE("play configuration acknowledgement cannot carry trailing fields") { malformedReconfigurationAck = true; }
    SUBCASE("unrelated backend identity is rejected") {
        finished[2] ^= 1;
        wrongProfile = true;
    }
    SUBCASE("login acknowledgement cannot carry trailing fields") { malformedAck = true; }
    SUBCASE("configuration acknowledgement requires the backend finish packet") { prematureFinishAck = true; }
    SUBCASE("configuration disconnect drains before backend EOF") { configurationDisconnect = true; }
    std::vector<std::uint8_t> packets{3, 3, 0x80, 2, 42, 0};
    packets.insert(packets.end(), finished.begin() + 1, finished.end());
    REQUIRE(backend->write(packets));
    if (wrongProfile) {
        CHECK(receive(client, 0, true).empty());
        proxy.stop();
        return;
    }
    CHECK(receive(client, finished.size()) == finished);
    if (loginEof) {
        backend->close();
        CHECK(receive(client, 0, true).empty());
        proxy.stop();
        return;
    }
    if (malformedAck) {
        const std::array<std::uint8_t, 3> badAck{2, 3, 0};
        REQUIRE(client.write(badAck));
        CHECK(receive(client, 0, true).empty());
        proxy.stop();
        return;
    }
    const std::array<std::uint8_t, 2> ack{1, 3};
    REQUIRE(client.write(ack));
    CHECK(receive(*backend, 3) == std::vector<std::uint8_t>{2, 0, 3});
    if (configurationDisconnect) {
        const std::vector<std::uint8_t> reason{8, 0, 2, 8, 0, 3, 'b', 'y', 'e'};
        REQUIRE(backend->write(reason));
        backend->close();
        CHECK(receive(client, 0, true) == std::vector<std::uint8_t>{7, 2, 8, 0, 3, 'b', 'y', 'e'});
        proxy.stop();
        return;
    }
    if (prematureFinishAck) {
        REQUIRE(client.write(ack));
        CHECK(receive(client, 0, true).empty());
        proxy.stop();
        return;
    }

    const std::vector<std::uint8_t> configuration{3, 0, 15, 0, 10, 0, 4, 0, 0, 0, 0, 0, 0, 0, 42};
    REQUIRE(backend->write(configuration));
    const std::array<std::uint8_t, 5> configurationPrefix{2, 15, 0, 9, 4};
    const auto frontendConfiguration = receive(client, 13);
    REQUIRE(frontendConfiguration.size() == 13);
    CHECK(std::equal(configurationPrefix.begin(), configurationPrefix.end(), frontendConfiguration.begin()));
    const std::array<std::uint8_t, 3> knownPacks{2, 7, 0};
    REQUIRE(client.write(knownPacks));
    CHECK(receive(*backend, 4) == std::vector<std::uint8_t>{3, 0, 7, 0});
    // Independent zlib vector for a large, valid minecraft:brand custom payload.
    const std::vector<std::uint8_t> largeBrand{43, 0xe3, 0x0f, 120, 156, 99,  228, 207, 205, 204, 75, 77,  46,  74,  76,
                                               43, 177,  74,   42,  74,  204, 75,  185, 192, 95,  49, 10,  70,  193, 40,
                                               24, 5,    163,  96,  20,  140, 130, 33,  15,  0,   13, 254, 176, 151};
    REQUIRE(backend->write(largeBrand));
    std::vector<std::uint8_t> expectedBrand{0xe3, 0x0f, 1, 15};
    const std::string brandChannel = "minecraft:brand";
    expectedBrand.insert(expectedBrand.end(), brandChannel.begin(), brandChannel.end());
    expectedBrand.insert(expectedBrand.end(), {0xd0, 0x0f});
    expectedBrand.insert(expectedBrand.end(), 2000, 'x');
    CHECK(receive(client, expectedBrand.size()) == expectedBrand);
    expectedBrand[2] = 2;
    REQUIRE(client.write(expectedBrand));
    const std::vector<std::uint8_t> expectedBackendBrand{
        43,  0xe3, 0x0f, 120, 156, 99, 226, 207, 205, 204, 75,  77, 46, 74,  76,  43, 177, 74, 42, 74,  204, 75,
        185, 192,  95,   49,  10,  70, 193, 40,  24,  5,   163, 96, 20, 140, 130, 33, 15,  0,  21, 225, 176, 152};
    CHECK(receive(*backend, expectedBackendBrand.size()) == expectedBackendBrand);
    const std::array<std::uint8_t, 3> finishConfiguration{2, 0, 3};
    REQUIRE(backend->write(finishConfiguration));
    CHECK(receive(client, 2) == std::vector<std::uint8_t>{1, 3});
    REQUIRE(client.write(ack));
    CHECK(receive(*backend, 3) == std::vector<std::uint8_t>{2, 0, 3});
    const std::vector<std::uint8_t> playKeepAlive{10, 0, 0x2d, 0, 0, 0, 0, 0, 0, 0, 99};
    REQUIRE(backend->write(playKeepAlive));
    auto keepAliveResponse = receive(client, 10);
    REQUIRE(keepAliveResponse.size() == 10);
    REQUIRE(keepAliveResponse[1] == 0x2d);
    keepAliveResponse[1] = 0x1c;
    REQUIRE(client.write(keepAliveResponse));
    CHECK(receive(*backend, 11) == std::vector<std::uint8_t>{10, 0, 0x1c, 0, 0, 0, 0, 0, 0, 0, 99});
    {
        namespace play = basalt::wire::protocol::play;
        namespace framing = basalt::wire::protocol::packetFraming;
        const auto relayed = [&](std::int32_t id, const auto& packet) {
            io::ByteBuffer body;
            basalt::wire::protocol::varInt::write(body, id);
            packet.write(body);
            io::ByteBuffer framed;
            REQUIRE(framing::writeUncompressed(framed, body.readable()));
            REQUIRE(client.write(framed.readable()));
            io::ByteBuffer wire;
            io::ByteBuffer frame;
            for (int attempts = 0; attempts < 20; ++attempts) {
                wire.writeBytes(receive(*backend, 1));
                const auto read = framing::tryReadFrame(wire, true, frame, 256);
                REQUIRE(read);
                if (*read)
                    break;
            }
            CHECK(frame.toVector() == body.toVector());
        };
        play::MessageSignature signature{};
        for (std::size_t i = 0; i < signature.size(); ++i)
            signature[i] = static_cast<std::uint8_t>(i * 7 + 3);
        play::LastSeenUpdate seen;
        seen.offset = 3;
        seen.setAcknowledged(1);
        seen.setAcknowledged(17);
        seen.checksum = 42;
        play::ChatMessagePacket chat;
        chat.message = "hello there";
        chat.timestamp = 1700000000123;
        chat.salt = 0x1122334455667788;
        chat.signature = signature;
        chat.lastSeen = seen;
        relayed(play::ChatMessagePacket::kId, chat);
        play::ChatSessionUpdatePacket session;
        session.session.sessionId = basalt::wire::common::Uuid::offlinePlayer("chat-session");
        session.session.expiresAt = 1800000000000;
        session.session.publicKey.assign(294, 0x5a);
        session.session.keySignature.assign(512, 0xa5);
        relayed(play::ChatSessionUpdatePacket::kId, session);
        play::SignedChatCommandPacket command;
        command.command = "msg Steve hi";
        command.timestamp = 1700000000456;
        command.salt = 99;
        command.argumentSignatures.push_back({"message", signature});
        command.lastSeen = seen;
        relayed(play::SignedChatCommandPacket::kId, command);
    }
    if (prematureReconfigurationAck) {
        REQUIRE(client.write(std::array<std::uint8_t, 2>{1, 0x10}));
        CHECK(receive(client, 0, true).empty());
        proxy.stop();
        return;
    }
    if (malformedStart) {
        REQUIRE(backend->write(std::array<std::uint8_t, 4>{3, 0, 0x78, 0}));
        CHECK(receive(client, 0, true).empty());
        proxy.stop();
        return;
    }
    for (int cycle = 0; cycle < 2; ++cycle) {
        std::vector<std::uint8_t> reconfiguration{2, 0, 0x78};
        reconfiguration.insert(reconfiguration.end(), configuration.begin(), configuration.end());
        REQUIRE(backend->write(reconfiguration));
        CHECK(receive(client, 2) == std::vector<std::uint8_t>{1, 0x78});
        if (reconfigurationEof) {
            backend->close();
            CHECK(receive(client, 0, true).empty());
            proxy.stop();
            return;
        }
        if (malformedReconfigurationAck) {
            REQUIRE(client.write(std::array<std::uint8_t, 3>{2, 0x10, 0}));
            CHECK(receive(client, 0, true).empty());
            proxy.stop();
            return;
        }
        REQUIRE(client.write(std::array<std::uint8_t, 2>{1, 0x10}));
        CHECK(receive(*backend, 3) == std::vector<std::uint8_t>{2, 0, 0x10});
        const auto reconfigured = receive(client, frontendConfiguration.size());
        REQUIRE(reconfigured.size() == frontendConfiguration.size());
        CHECK(std::equal(configurationPrefix.begin(), configurationPrefix.end(), reconfigured.begin()));
        CHECK_FALSE(std::equal(reconfigured.end() - 8, reconfigured.end(), frontendConfiguration.end() - 8));
        REQUIRE(backend->write(finishConfiguration));
        CHECK(receive(client, 2) == std::vector<std::uint8_t>{1, 3});
        REQUIRE(client.write(ack));
        CHECK(receive(*backend, 3) == std::vector<std::uint8_t>{2, 0, 3});
        REQUIRE(backend->write(playKeepAlive));
        const auto published = receive(client, 10);
        REQUIRE(published.size() == 10);
        CHECK(published[1] == 0x2d);
    }
    const std::vector<std::uint8_t> disconnect{8, 0, 0x20, 8, 0, 3, 'b', 'y', 'e'};
    REQUIRE(backend->write(disconnect));
    backend->close();
    CHECK(receive(client, 0, true) == std::vector<std::uint8_t>{7, 0x20, 8, 0, 3, 'b', 'y', 'e'});
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("Velocity modern forwarding signs verified identity and requires query before success") {
    auto config = settings();
    config.network.onlineMode = false;
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    config.backends = {{.name = "modern",
                        .host = "127.0.0.1",
                        .port = *port,
                        .forwarding = basalt::wire::auth::forwarding::ForwardingMode::Velocity,
                        .secret = "fixture-secret"}};
    config.defaultRoute = {"modern"};
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
    unsigned requested = 4;
    bool missing = false;
    bool duplicate = false;
    bool malformed = false;
    bool largeProfile = false;
    bool excessiveProperties = false;
    SUBCASE("profile properties larger than bootstrap are relayed") { largeProfile = true; }
    SUBCASE("excessive profile collection is rejected before allocation") { excessiveProperties = true; }
    SUBCASE("v4") {}
    SUBCASE("v1") { requested = 1; }
    SUBCASE("native clients negotiate v1 when v3 signing keys are requested") { requested = 3; }
    SUBCASE("success without required forwarding is rejected") { missing = true; }
    SUBCASE("a repeated velocity query is answered again, as Velocity does") { duplicate = true; }
    SUBCASE("zero forwarding version is rejected") {
        malformed = true;
        requested = 0;
    }
    std::vector<std::uint8_t> finished{41,   2,    0xf3, 0xd2, 0x8c, 0xb0, 0x72, 0x25, 0x3c, 0xb1, 0xba, 0xeb, 0x2d,
                                       0xad, 0xd2, 0xbe, 0x89, 0xae, 6,    'T',  'e',  's',  't',  'e',  'r',  0};
    finished.insert(finished.end(), 15, 0);
    finished.push_back(1);
    if (!missing) {
        const std::string channel = "velocity:player_info";
        std::vector<std::uint8_t> request{24, 4, 42, 20};
        request.insert(request.end(), channel.begin(), channel.end());
        request.push_back(static_cast<std::uint8_t>(requested));
        REQUIRE(backend->write(request));
        if (malformed) {
            CHECK(receive(client, 0, true).empty());
            proxy.stop();
            CHECK(proxy.runtime.sessionCount() == 0);
            CHECK(proxy.runtime.bufferedBytes() == 0);
            return;
        }
        std::vector<std::uint8_t> expected{70, 2, 42, 1};
        const std::vector<std::uint8_t> tag =
            requested >= 4 ? std::vector<std::uint8_t>{142, 62,  4,   137, 254, 120, 108, 171, 34,  177, 250,
                                                       233, 252, 216, 113, 254, 106, 58,  150, 181, 83,  64,
                                                       213, 80,  37,  87,  7,   11,  36,  27,  86,  70}
                           : std::vector<std::uint8_t>{54,  0,   252, 43,  139, 165, 238, 43,  253, 5,   4,
                                                       56,  226, 217, 177, 170, 252, 101, 221, 237, 181, 205,
                                                       102, 255, 219, 194, 158, 151, 136, 101, 106, 110};
        expected.insert(expected.end(), tag.begin(), tag.end());
        expected.insert(expected.end(), {static_cast<std::uint8_t>(requested >= 4 ? 4 : 1), 9});
        const std::string ip = "127.0.0.1";
        expected.insert(expected.end(), ip.begin(), ip.end());
        expected.insert(expected.end(), finished.begin() + 2, finished.begin() + 18);
        expected.insert(expected.end(), {6, 'T', 'e', 's', 't', 'e', 'r', 0});
        CHECK(receive(*backend, expected.size()) == expected);
        if (duplicate) {
            REQUIRE(backend->write(request));
            CHECK(receive(*backend, expected.size()) == expected);
            proxy.stop();
            CHECK(proxy.runtime.sessionCount() == 0);
            CHECK(proxy.runtime.bufferedBytes() == 0);
            return;
        }
    }
    if (excessiveProperties)
        finished[25] = 65;
    if (largeProfile) {
        // Independent wire vector: one unsigned textures property with a 2 KiB value.
        finished.erase(finished.begin(), finished.begin() + 1);
        finished[24] = 1;
        finished.insert(finished.begin() + 25, {8, 't', 'e', 'x', 't', 'u', 'r', 'e', 's', 0x80, 0x10});
        finished.insert(finished.begin() + 36, 2048, 'a');
        finished.insert(finished.begin() + 2084, 0);
        const auto size = finished.size();
        finished.insert(finished.begin(),
                        {static_cast<std::uint8_t>((size & 127) | 128), static_cast<std::uint8_t>(size >> 7)});
    }
    REQUIRE(backend->write(finished));
    if (missing || excessiveProperties)
        CHECK(receive(client, 0, true).empty());
    else
        CHECK(receive(client, finished.size()) == finished);
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}
