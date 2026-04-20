#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "RuntimeFixture.hpp"

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
    bool wrongProfile = false;
    bool malformedAck = false;
    bool prematureFinishAck = false;
    bool configurationDisconnect = false;
    bool malformedStart = false;
    bool prematureReconfigurationAck = false;
    bool malformedReconfigurationAck = false;
    SUBCASE("valid native transition") {}
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

    // Coalesced known-packs and keepalive must both drain without another backend read.
    const std::vector<std::uint8_t> configuration{3, 0, 15, 0, 10, 0, 4, 0, 0, 0, 0, 0, 0, 0, 42};
    REQUIRE(backend->write(configuration));
    const std::vector<std::uint8_t> frontendConfiguration{2, 15, 0, 9, 4, 0, 0, 0, 0, 0, 0, 0, 42};
    CHECK(receive(client, frontendConfiguration.size()) == frontendConfiguration);
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
    CHECK(receive(client, 10) == std::vector<std::uint8_t>{9, 0x2d, 0, 0, 0, 0, 0, 0, 0, 99});
    const std::vector<std::uint8_t> keepAliveResponse{9, 0x1c, 0, 0, 0, 0, 0, 0, 0, 99};
    REQUIRE(client.write(keepAliveResponse));
    CHECK(receive(*backend, 11) == std::vector<std::uint8_t>{10, 0, 0x1c, 0, 0, 0, 0, 0, 0, 0, 99});
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
        if (malformedReconfigurationAck) {
            REQUIRE(client.write(std::array<std::uint8_t, 3>{2, 0x10, 0}));
            CHECK(receive(client, 0, true).empty());
            proxy.stop();
            return;
        }
        REQUIRE(client.write(std::array<std::uint8_t, 2>{1, 0x10}));
        CHECK(receive(*backend, 3) == std::vector<std::uint8_t>{2, 0, 0x10});
        CHECK(receive(client, frontendConfiguration.size()) == frontendConfiguration);
        REQUIRE(backend->write(finishConfiguration));
        CHECK(receive(client, 2) == std::vector<std::uint8_t>{1, 3});
        REQUIRE(client.write(ack));
        CHECK(receive(*backend, 3) == std::vector<std::uint8_t>{2, 0, 3});
        REQUIRE(backend->write(playKeepAlive));
        CHECK(receive(client, 10) == std::vector<std::uint8_t>{9, 0x2d, 0, 0, 0, 0, 0, 0, 0, 99});
    }
    const std::vector<std::uint8_t> disconnect{8, 0, 0x20, 8, 0, 3, 'b', 'y', 'e'};
    REQUIRE(backend->write(disconnect));
    backend->close();
    CHECK(receive(client, 0, true) == std::vector<std::uint8_t>{7, 0x20, 8, 0, 3, 'b', 'y', 'e'});
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("online login preserves frontend identity and cipher state across configuration ACKs") {
    using namespace basalt::wire::auth;
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    const auto port = listener->localPort();
    REQUIRE(port);
    auto config = settings();
    config.backends = {{.name = "offline-backend", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"offline-backend"};
    runtime::RuntimeServices services;
    services.authentication = [](auto, auto, auto) {
        const std::string json = "{\"id\":\"0123456789abcdef0123456789abcdef\",\"name\":\"Tester\"}";
        return std::expected<HttpsResponse, HttpsError>{HttpsResponse{200, {json.begin(), json.end()}}};
    };
    Running proxy{std::move(config), std::move(services)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    const auto secret = keyResponse(client);
    auto encryptor = Cfb8Cipher::create(secret);
    auto decryptor = Cfb8Cipher::create(secret);
    REQUIRE(encryptor);
    REQUIRE(decryptor);
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
    REQUIRE(backend->write(finished));
    auto frontendFinished = receive(client, 42);
    decryptor->decrypt(frontendFinished);
    const std::array<std::uint8_t, 16> verifiedUuid{1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                                    1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    std::copy(verifiedUuid.begin(), verifiedUuid.end(), finished.begin() + 2);
    CHECK(frontendFinished == finished);
    std::array<std::uint8_t, 2> ack{1, 3};
    encryptor->encrypt(ack);
    REQUIRE(client.write(ack));
    CHECK(receive(*backend, 2) == std::vector<std::uint8_t>{1, 3});
    const std::array<std::uint8_t, 3> packs{2, 15, 0};
    REQUIRE(backend->write(packs));
    auto receivedPacks = receive(client, 3);
    decryptor->decrypt(receivedPacks);
    CHECK(receivedPacks == std::vector<std::uint8_t>{2, 15, 0});
    std::array<std::uint8_t, 3> answer{2, 7, 0};
    encryptor->encrypt(answer);
    REQUIRE(client.write(answer));
    CHECK(receive(*backend, 3) == std::vector<std::uint8_t>{2, 7, 0});
    const std::array<std::uint8_t, 2> finish{1, 3};
    REQUIRE(backend->write(finish));
    auto receivedFinish = receive(client, 2);
    decryptor->decrypt(receivedFinish);
    CHECK(receivedFinish == std::vector<std::uint8_t>{1, 3});
    ack = {1, 3};
    encryptor->encrypt(ack);
    REQUIRE(client.write(ack));
    CHECK(receive(*backend, 2) == std::vector<std::uint8_t>{1, 3});
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("small session quota rejects routing without blocking another frontend") {
    auto config = settings();
    config.network.onlineMode = false;
    config.runtime.sessionBufferBytes = 8192;
    config.runtime.highWaterBytes = 4096;
    config.runtime.lowWaterBytes = 2048;
    config.backends = {{.name = "limited", .host = "limited.test"}};
    config.defaultRoute = {"limited"};
    std::atomic<unsigned> queries{0};
    runtime::RuntimeServices services;
    services.resolution = [&](auto, auto, const auto&) -> std::expected<std::vector<io::TcpAddress>, io::DnsError> {
        queries.fetch_add(1);
        return std::unexpected(io::DnsError{io::DnsError::Kind::ResolveFailed});
    };
    Running proxy{std::move(config), std::move(services)};
    auto limited = proxy.connect();
    loginStart(limited, proxy.runtime.port());
    CHECK(receive(limited, 0, true).empty());
    CHECK(queries.load() == 0);
    auto healthy = proxy.connect();
    REQUIRE(healthy.write(handshake(proxy.runtime.port())));
    const std::array<std::uint8_t, 2> status{1, 0};
    REQUIRE(healthy.write(status));
    CHECK_FALSE(receive(healthy, 1).empty());
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("large configuration payload grows within quota or rejects its header without a body") {
    auto config = settings();
    config.network.onlineMode = false;
    config.runtime.highWaterBytes = 64;
    config.runtime.lowWaterBytes = 32;
    bool limited = false;
    SUBCASE("valid half-megabyte custom payload") {}
    SUBCASE("local quota stops reservation before receiving the body") {
        limited = true;
        config.runtime.sessionBufferBytes = 32768;
        config.runtime.highWaterBytes = 16384;
        config.runtime.lowWaterBytes = 8192;
    }
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    config.backends = {{.name = "large", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"large"};
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
    REQUIRE(backend->write(finished));
    CHECK(receive(client, 42) == finished);
    const std::array<std::uint8_t, 2> ack{1, 3};
    REQUIRE(client.write(ack));
    CHECK(receive(*backend, 2) == std::vector<std::uint8_t>{1, 3});
    // 524302-byte body: configuration payload id 1, channel fixture:bulk, 512 KiB bytes.
    std::vector<std::uint8_t> packet{0x8e, 0x80, 0x20};
    REQUIRE(backend->write(packet));
    if (limited) {
        CHECK(receive(client, 0, true).empty());
        auto healthy = proxy.connect();
        REQUIRE(healthy.write(handshake(proxy.runtime.port())));
        const std::array<std::uint8_t, 2> status{1, 0};
        REQUIRE(healthy.write(status));
        CHECK_FALSE(receive(healthy, 1).empty());
    } else {
        const std::string channel = "fixture:bulk";
        std::vector<std::uint8_t> body{1, 12};
        body.insert(body.end(), channel.begin(), channel.end());
        body.insert(body.end(), 512 * 1024, 42);
        REQUIRE(backend->write(body));
        packet.insert(packet.end(), body.begin(), body.end());
        CHECK(receive(client, packet.size()) == packet);
        packet[3] = 2;
        REQUIRE(client.write(packet));
        CHECK(receive(*backend, packet.size()) == packet);
        std::vector<std::uint8_t> pipelined;
        for (unsigned i = 0; i < 2000; ++i)
            pipelined.insert(pipelined.end(), {2, 7, 0});
        REQUIRE(client.write(pipelined));
        CHECK(receive(*backend, pipelined.size()) == pipelined);
    }
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("frontend compression is negotiated before success independently of backend framing") {
    using namespace basalt::wire::auth;
    auto config = settings();
    config.network.onlineMode = false;
    config.network.compressionThreshold = 256;
    bool wrongFraming = false;
    SUBCASE("under-threshold carrier") {}
    SUBCASE("threshold zero uses zlib") { config.network.compressionThreshold = 0; }
    SUBCASE("an uncompressed ACK cannot bypass negotiated framing") { wrongFraming = true; }
    SUBCASE("cipher wraps compressed frames and negotiation in stream order") {
        config.network.compressionThreshold = 0;
        config.network.onlineMode = true;
    }
    const bool compressed = config.network.compressionThreshold == 0;
    const bool online = config.network.onlineMode;
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    config.backends = {{.name = "plain", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"plain"};
    runtime::RuntimeServices services;
    services.authentication = [](auto, auto, auto) { return std::expected<HttpsResponse, HttpsError>{fakeProfile()}; };
    Running proxy{std::move(config), std::move(services)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    std::optional<Cfb8Cipher> encryptor, decryptor;
    if (online) {
        auto secret = keyResponse(client);
        encryptor = Cfb8Cipher::create(secret);
        decryptor = Cfb8Cipher::create(secret);
        REQUIRE(encryptor);
        REQUIRE(decryptor);
    }
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
    REQUIRE(backend->write(finished));
    std::vector<std::uint8_t> expected;
    if (compressed) {
        // Python zlib vectors, independent of the Wire encoders.
        expected = {2,   3,   0,   38,  41,  120, 156, 99, 250, 124, 169, 103, 67, 145,
                    170, 205, 198, 93,  175, 117, 215, 94, 218, 215, 185, 142, 45, 36,
                    181, 184, 36,  181, 136, 1,   13,  48, 2,   0,   113, 227, 12, 76};
    } else {
        expected = {3, 3, 0x80, 2, 42, 0};
        expected.insert(expected.end(), finished.begin() + 1, finished.end());
    }
    auto received = receive(client, expected.size());
    if (decryptor)
        decryptor->decrypt(received);
    CHECK(received == expected);
    if (wrongFraming) {
        const std::array<std::uint8_t, 2> invalidAck{1, 3};
        REQUIRE(client.write(invalidAck));
        CHECK(receive(client, 0, true).empty());
        proxy.stop();
        CHECK(proxy.runtime.sessionCount() == 0);
        CHECK(proxy.runtime.bufferedBytes() == 0);
        return;
    }
    auto ack = compressed ? std::vector<std::uint8_t>{10, 1, 120, 156, 99, 6, 0, 0, 4, 0, 4}
                          : std::vector<std::uint8_t>{2, 0, 3};
    if (encryptor)
        encryptor->encrypt(ack);
    REQUIRE(client.write(ack));
    CHECK(receive(*backend, 2) == std::vector<std::uint8_t>{1, 3});
    const std::array<std::uint8_t, 3> packs{2, 15, 0};
    REQUIRE(backend->write(packs));
    expected = compressed ? std::vector<std::uint8_t>{11, 2, 120, 156, 227, 103, 0, 0, 0, 32, 0, 16}
                          : std::vector<std::uint8_t>{3, 0, 15, 0};
    received = receive(client, expected.size());
    if (decryptor)
        decryptor->decrypt(received);
    CHECK(received == expected);
    auto response = compressed ? std::vector<std::uint8_t>{11, 2, 120, 156, 99, 103, 0, 0, 0, 16, 0, 8}
                               : std::vector<std::uint8_t>{3, 0, 7, 0};
    if (encryptor)
        encryptor->encrypt(response);
    REQUIRE(client.write(response));
    CHECK(receive(*backend, 3) == std::vector<std::uint8_t>{2, 7, 0});
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("backend login cookie request is matched to a bounded frontend response") {
    auto config = settings();
    config.network.onlineMode = false;
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    config.backends = {{.name = "cookies", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"cookies"};
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
    const std::string key = "fixture:cookie";
    std::vector<std::uint8_t> request{16, 5, 14};
    request.insert(request.end(), key.begin(), key.end());
    REQUIRE(backend->write(request));
    CHECK(receive(client, request.size()) == request);
    std::vector<std::uint8_t> response;
    bool rejected = false;
    SUBCASE("absent payload") {
        response = {17, 4, 14};
        response.insert(response.end(), key.begin(), key.end());
        response.push_back(0);
    }
    SUBCASE("maximum payload crosses the bootstrap buffer") {
        response = {0x93, 0x28, 4, 14};
        response.insert(response.end(), key.begin(), key.end());
        response.insert(response.end(), {1, 0x80, 0x28});
        response.insert(response.end(), 5120, 42);
    }
    SUBCASE("unrelated key cannot answer the pending query") {
        const std::string other = "fixture:other";
        response = {16, 4, 13};
        response.insert(response.end(), other.begin(), other.end());
        response.push_back(0);
        rejected = true;
    }
    REQUIRE(client.write(response));
    if (rejected)
        CHECK(receive(client, 0, true).empty());
    else
        CHECK(receive(*backend, response.size()) == response);
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
    SUBCASE("duplicate forwarding query is rejected") { duplicate = true; }
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
            CHECK(receive(client, 0, true).empty());
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

TEST_CASE("legacy forwarding handshakes carry the configured identity and token") {
    using Mode = basalt::wire::auth::forwarding::ForwardingMode;
    auto mode = Mode::BungeeCord;
    std::string property;
    std::string token = "fixture-secret";
    SUBCASE("BungeeCord") {}
    SUBCASE("BungeeGuard") {
        mode = Mode::BungeeGuard;
        property = "bungeeguard-token";
    }
    SUBCASE("SafeNET custom property") {
        mode = Mode::SafeNet;
        property = "fixture_token";
    }
    SUBCASE("BungeeGuard token exceeds bootstrap capacity") {
        mode = Mode::BungeeGuard;
        property = "bungeeguard-token";
        token.assign(2048, 'x');
    }
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    auto config = settings();
    config.network.onlineMode = false;
    config.backends = {{.name = "legacy",
                        .host = "127.0.0.1",
                        .port = *port,
                        .forwarding = mode,
                        .secret = property.empty() ? "" : token,
                        .propertyName = property.empty() ? "safe_net_passphrase" : property}};
    config.defaultRoute = {"legacy"};
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
    std::string host = "localhost";
    host.push_back('\0');
    host += "127.0.0.1";
    host.push_back('\0');
    host += "f3d28cb072253cb1baeb2dadd2be89ae";
    host.push_back('\0');
    host +=
        property.empty() ? "[]" : "[{\"name\":\"" + property + "\",\"value\":\"" + token + "\",\"signature\":\"\"}]";
    const auto appendInteger = [](std::vector<std::uint8_t>& bytes, std::size_t value) {
        do {
            auto byte = static_cast<std::uint8_t>(value & 127);
            value >>= 7;
            bytes.push_back(static_cast<std::uint8_t>(byte | (value ? 128 : 0)));
        } while (value);
    };
    std::vector<std::uint8_t> body{0, 0x89, 0x06};
    appendInteger(body, host.size());
    body.insert(body.end(), host.begin(), host.end());
    body.insert(body.end(), {static_cast<std::uint8_t>(proxy.runtime.port() >> 8),
                             static_cast<std::uint8_t>(proxy.runtime.port() & 255), 2});
    std::vector<std::uint8_t> expected;
    appendInteger(expected, body.size());
    expected.insert(expected.end(), body.begin(), body.end());
    expected.insert(expected.end(), {24,   0,    6,    'T',  'e',  's',  't',  'e',  'r',  0xf3, 0xd2, 0x8c, 0xb0,
                                     0x72, 0x25, 0x3c, 0xb1, 0xba, 0xeb, 0x2d, 0xad, 0xd2, 0xbe, 0x89, 0xae});
    CHECK(receive(*backend, expected.size()) == expected);
    std::vector<std::uint8_t> finished{2};
    finished.insert(finished.end(), expected.end() - 16, expected.end());
    finished.insert(finished.end(), {6, 'T', 'e', 's', 't', 'e', 'r', 0});
    finished.insert(finished.end(), 16, 0);
    std::vector<std::uint8_t> frontend{41};
    frontend.insert(frontend.end(), finished.begin(), finished.end());
    if (!property.empty()) {
        finished[24] = 1;
        std::vector<std::uint8_t> entry;
        appendInteger(entry, property.size());
        entry.insert(entry.end(), property.begin(), property.end());
        appendInteger(entry, token.size());
        entry.insert(entry.end(), token.begin(), token.end());
        entry.push_back(0);
        finished.insert(finished.begin() + 25, entry.begin(), entry.end());
    }
    std::vector<std::uint8_t> response;
    appendInteger(response, finished.size());
    response.insert(response.end(), finished.begin(), finished.end());
    REQUIRE(backend->write(response));
    // A backend echo of the credential property must be removed from frontend LoginFinished.
    CHECK(receive(client, frontend.size()) == frontend);
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}
