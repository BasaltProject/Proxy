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

TEST_CASE("basalt forwarding answers the backend challenge with a verifiable signature") {
    namespace native = basalt::wire::auth::forwarding::native;
    const std::string keyFile = "basalt-forwarding-login-test.key";
    std::filesystem::remove(keyFile);
    auto config = settings();
    config.network.onlineMode = false;
    config.forwarding.keyFile = keyFile;
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    config.backends = {{.name = "native",
                        .host = "127.0.0.1",
                        .port = *port,
                        .forwarding = basalt::wire::auth::forwarding::ForwardingMode::Basalt}};
    config.defaultRoute = {"native"};
    Running proxy{std::move(config)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    io::EventLoop loop;
    REQUIRE(loop.watch(listener->nativeHandle(), {true, false}, 1));
    auto events = loop.wait(2000);
    REQUIRE(events);
    auto backend = listener->accept();
    REQUIRE(backend);
    REQUIRE(backend->setNonBlocking());
    REQUIRE(receive(*backend, 42).size() == 42);

    const native::Request issued = *native::newRequest("lobby");
    const auto payload = native::encodeRequest(issued);
    const std::string channel{native::kChannel};
    std::vector<std::uint8_t> body{4, 42, static_cast<std::uint8_t>(channel.size())};
    body.insert(body.end(), channel.begin(), channel.end());
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<std::uint8_t> request{static_cast<std::uint8_t>(body.size())};
    request.insert(request.end(), body.begin(), body.end());
    REQUIRE(backend->write(request));

    native::Identity probe;
    probe.name = "Tester";
    probe.address = "127.0.0.1";
    probe.proxyId = "proxy";
    probe.virtualHost = "localhost";
    const std::size_t responseBytes =
        native::sign(native::SigningKey::fromSeed(std::array<std::uint8_t, 32>{}), issued, native::kVersion1, probe)
            .size();
    const std::size_t packetBytes = 1 + 1 + 1 + responseBytes;
    const std::size_t prefixBytes = packetBytes > 127 ? 2 : 1;
    const auto received = receive(*backend, prefixBytes + packetBytes);
    REQUIRE(received.size() == prefixBytes + packetBytes);
    CHECK(received[prefixBytes] == 2);
    CHECK(received[prefixBytes + 1] == 42);
    CHECK(received[prefixBytes + 2] == 1);

    std::ifstream stored{keyFile};
    std::string text;
    stored >> text;
    stored.close();
    REQUIRE(text.size() == 64);
    std::array<std::uint8_t, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<std::uint8_t>(std::stoi(text.substr(2 * i, 2), nullptr, 16));
    const std::array<native::PublicKey, 1> trusted = {native::SigningKey::fromSeed(seed).publicKey()};
    const auto verified = native::verify(std::span{received}.subspan(prefixBytes + 3), issued, trusted);
    REQUIRE(std::holds_alternative<native::Verified>(verified));
    const auto& identity = std::get<native::Verified>(verified).identity;
    CHECK(identity.name == "Tester");
    CHECK(identity.address == "127.0.0.1");
    CHECK(identity.proxyId == "proxy");
    CHECK(identity.port != 0);
    CHECK_FALSE(identity.onlineMode);
    proxy.stop();
    std::filesystem::remove(keyFile);
}

TEST_CASE("a basalt backend that offers the session envelope gets Open, data frames and a close on a malformed frame") {
    namespace native = basalt::wire::auth::forwarding::native;
    namespace envelope = basalt::wire::protocol::sessionEnvelope;
    const std::string keyFile = "basalt-envelope-login-test.key";
    std::filesystem::remove(keyFile);
    auto config = settings();
    config.network.onlineMode = false;
    config.forwarding.keyFile = keyFile;
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    config.backends = {{.name = "native",
                        .host = "127.0.0.1",
                        .port = *port,
                        .protocol = basalt::proxy::config::BackendConfig::kClientProtocol,
                        .forwarding = basalt::wire::auth::forwarding::ForwardingMode::Basalt}};
    config.defaultRoute = {"native"};
    Running proxy{std::move(config)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    io::EventLoop loop;
    REQUIRE(loop.watch(listener->nativeHandle(), {true, false}, 1));
    REQUIRE(loop.wait(2000));
    auto backend = listener->accept();
    REQUIRE(backend);
    REQUIRE(backend->setNonBlocking());
    REQUIRE(receive(*backend, 42).size() == 42);

    native::Request issued = *native::newRequest("lobby");
    issued.capabilities = native::kCapabilitySessionEnvelope;
    const auto payload = native::encodeRequest(issued);
    const std::string channel{native::kChannel};
    std::vector<std::uint8_t> body{4, 42, static_cast<std::uint8_t>(channel.size())};
    body.insert(body.end(), channel.begin(), channel.end());
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<std::uint8_t> request{static_cast<std::uint8_t>(body.size())};
    request.insert(request.end(), body.begin(), body.end());
    REQUIRE(backend->write(request));
    native::Identity probe;
    probe.name = "Tester";
    probe.address = "127.0.0.1";
    probe.proxyId = "proxy";
    probe.virtualHost = "localhost";
    probe.capabilities = native::kCapabilitySessionEnvelope;
    const std::size_t packetBytes =
        3 + native::sign(native::SigningKey::fromSeed(std::array<std::uint8_t, 32>{}), issued, native::kVersion1, probe)
                .size();
    const std::size_t prefixBytes = packetBytes > 127 ? 2 : 1;
    const auto answered = receive(*backend, prefixBytes + packetBytes);
    REQUIRE(answered.size() == prefixBytes + packetBytes);

    std::ifstream stored{keyFile};
    std::string text;
    stored >> text;
    stored.close();
    std::array<std::uint8_t, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<std::uint8_t>(std::stoi(text.substr(2 * i, 2), nullptr, 16));
    const std::array<native::PublicKey, 1> trusted = {native::SigningKey::fromSeed(seed).publicKey()};
    const auto verified = native::verify(std::span{answered}.subspan(prefixBytes + 3), issued, trusted);
    REQUIRE(std::holds_alternative<native::Verified>(verified));
    CHECK(std::get<native::Verified>(verified).identity.capabilities == native::kCapabilitySessionEnvelope);

    std::vector<std::uint8_t> finished{41,   2,    0xf3, 0xd2, 0x8c, 0xb0, 0x72, 0x25, 0x3c, 0xb1, 0xba, 0xeb, 0x2d,
                                       0xad, 0xd2, 0xbe, 0x89, 0xae, 6,    'T',  'e',  's',  't',  'e',  'r',  0};
    finished.insert(finished.end(), 15, 0);
    finished.push_back(1);
    REQUIRE(backend->write(finished));
    const auto expectedOpen =
        envelope::encodeControl(envelope::Open{envelope::kVersion1, envelope::Edition::Java, 777});
    const auto opened = receive(*backend, 1 + expectedOpen.size());
    REQUIRE(opened.size() == 1 + expectedOpen.size());
    const auto decoded = envelope::decode(std::span{opened}.subspan(1));
    REQUIRE(decoded);
    REQUIRE(std::holds_alternative<envelope::Control>(*decoded));
    CHECK(std::holds_alternative<envelope::Open>(std::get<envelope::Control>(*decoded)));

    SUBCASE("a frame that is neither Opened nor Close ends the login") {
        CHECK(receive(client, finished.size()) == finished);
        const std::array<std::uint8_t, 2> ack{1, 3};
        REQUIRE(client.write(ack));
        CHECK(receive(*backend, 3) == std::vector<std::uint8_t>{2, 0, 3});
        const std::vector<std::uint8_t> malformed{2, 9, 0};
        REQUIRE(backend->write(malformed));
        CHECK(receive(client, 0, true).empty());
    }
    SUBCASE("after Opened the packets of the client travel as data frames") {
        const auto answer = envelope::encodeControl(envelope::Opened{});
        std::vector<std::uint8_t> frame{static_cast<std::uint8_t>(answer.size())};
        frame.insert(frame.end(), answer.begin(), answer.end());
        REQUIRE(backend->write(frame));
        const std::array<std::uint8_t, 2> ack{1, 3};
        REQUIRE(client.write(ack));
        CHECK(receive(*backend, 3) == std::vector<std::uint8_t>{2, 0, 3});
    }
    proxy.stop();
    std::filesystem::remove(keyFile);
}

TEST_CASE("online login forwards the Mojang profile and its properties into the signed backend identity") {
    namespace velocity = basalt::wire::auth::forwarding::velocity;
    const auto id = basalt::wire::common::Uuid::offlinePlayer("Tester");
    const std::string json = "{\"id\":\"" + id.toUndashedString() +
                             "\",\"name\":\"Tester\",\"properties\":[{\"name\":\"textures\",\"value\":"
                             "\"dGV4dHVyZXM=\",\"signature\":\"c2ln\"}]}";
    runtime::RuntimeServices services{[&](auto, auto, auto) {
        return std::expected<basalt::wire::auth::HttpsResponse, basalt::wire::auth::HttpsError>{
            basalt::wire::auth::HttpsResponse{200, {json.begin(), json.end()}}};
    }};
    auto config = settings();
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
    Running proxy{std::move(config), std::move(services)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    static_cast<void>(keyResponse(client));
    io::EventLoop loop;
    REQUIRE(loop.watch(listener->nativeHandle(), {true, false}, 1));
    auto events = loop.wait(3000);
    REQUIRE(events);
    REQUIRE_FALSE(events->empty());
    auto backend = listener->accept();
    REQUIRE(backend);
    REQUIRE(backend->setNonBlocking());
    REQUIRE(receive(*backend, 42).size() == 42);
    const std::string channel = "velocity:player_info";
    std::vector<std::uint8_t> request{24, 4, 42, 20};
    request.insert(request.end(), channel.begin(), channel.end());
    request.push_back(4);
    REQUIRE(backend->write(request));

    const std::vector<basalt::wire::auth::SessionProfileProperty> properties{{"textures", "dGV4dHVyZXM=", "c2ln"}};
    const auto payload = velocity::sign("fixture-secret", "127.0.0.1", id, "Tester", properties, 4);
    REQUIRE_FALSE(payload.empty());
    std::vector<std::uint8_t> expected{
        static_cast<std::uint8_t>(3 + payload.size() > 127 ? (3 + payload.size()) | 128 : 3 + payload.size())};
    if (3 + payload.size() > 127)
        expected.push_back(static_cast<std::uint8_t>((3 + payload.size()) >> 7));
    expected.insert(expected.end(), {2, 42, 1});
    expected.insert(expected.end(), payload.begin(), payload.end());
    CHECK(receive(*backend, expected.size()) == expected);
    proxy.stop();
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
    CHECK(receive(client, frontend.size()) == frontend);
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}

TEST_CASE("a Bedrock player a basalt backend is asked to serve gets Open and its identity right after the login") {
    namespace native = basalt::wire::auth::forwarding::native;
    namespace envelope = basalt::wire::protocol::sessionEnvelope;
    namespace detail = basalt::proxy::runtime::detail;
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    auto port = listener->localPort();
    REQUIRE(port);
    auto connected = io::TcpConnection::connect("127.0.0.1", *port);
    REQUIRE(connected);
    REQUIRE(connected->setNonBlocking());
    auto backend = listener->accept();
    REQUIRE(backend);
    REQUIRE(backend->setNonBlocking());

    detail::BackendLogin login{std::move(*connected), *std::pmr::new_delete_resource(), {1, 0}};
    basalt::proxy::config::BackendConfig config{.name = "b", .host = "127.0.0.1", .port = *port};
    config.forwarding = basalt::wire::auth::forwarding::ForwardingMode::Basalt;
    basalt::wire::auth::SessionProfile profile{basalt::wire::common::Uuid::offlinePlayer("Tester"), "Tester", {}};
    detail::NativeForwarding forwarding{native::SigningKey::fromSeed(std::array<std::uint8_t, 32>{}), "proxy", false,
                                        true, true};
    envelope::BedrockIdentity identity;
    identity.gamertag = "Tester";
    identity.xuid = "2535";
    identity.address = "127.0.0.1";
    identity.skinId = "skin";
    identity.skinWidth = 1;
    identity.skinHeight = 1;
    identity.skinPixels = {1, 2, 3, 4};
    login.useOrigin(1, "2535");
    login.delegateBedrock(identity, 2193);
    REQUIRE(login.start(config, profile, "localhost", 25565, "127.0.0.1", 40000, &forwarding));

    const io::IoReady ready{.handle = login.handle(), .readable = true, .writable = true};
    const auto pump = [&] { return login.ready(ready); };
    const auto drain = [&](io::TcpConnection& side) {
        std::vector<std::uint8_t> all;
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            std::array<std::uint8_t, 4096> chunk{};
            const auto read = side.read(chunk);
            if (read && *read > 0)
                all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*read));
        }
        return all;
    };
    static_cast<void>(pump());
    static_cast<void>(drain(*backend));

    const native::Request issued = [] {
        auto request = *native::newRequest("lobby");
        request.capabilities = native::kCapabilitySessionEnvelope;
        return request;
    }();
    const auto payload = native::encodeRequest(issued);
    const std::string channel{native::kChannel};
    std::vector<std::uint8_t> body{4, 42, static_cast<std::uint8_t>(channel.size())};
    body.insert(body.end(), channel.begin(), channel.end());
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<std::uint8_t> query{static_cast<std::uint8_t>(body.size())};
    query.insert(query.end(), body.begin(), body.end());
    REQUIRE(backend->write(query));
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    const auto asked = pump();
    REQUIRE(asked);
    REQUIRE_FALSE(asked->empty());
    basalt::wire::io::ByteBuffer questionBody;
    questionBody.writeBytes(*asked);
    REQUIRE(basalt::wire::protocol::varInt::read(questionBody) == 4);
    REQUIRE(login.answerPluginQuery(questionBody));
    static_cast<void>(pump());
    static_cast<void>(drain(*backend));

    std::vector<std::uint8_t> finished{41,   2,    0xf3, 0xd2, 0x8c, 0xb0, 0x72, 0x25, 0x3c, 0xb1, 0xba, 0xeb, 0x2d,
                                       0xad, 0xd2, 0xbe, 0x89, 0xae, 6,    'T',  'e',  's',  't',  'e',  'r',  0};
    finished.insert(finished.end(), 15, 0);
    finished.push_back(1);
    REQUIRE(backend->write(finished));
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    const auto packet = pump();
    REQUIRE(packet);
    REQUIRE_FALSE(packet->empty());
    basalt::wire::io::ByteBuffer frame;
    frame.writeBytes(*packet);
    REQUIRE(basalt::wire::protocol::varInt::read(frame) == 2);
    REQUIRE(login.acceptLogin(frame));
    CHECK(login.delegated());

    static_cast<void>(pump());
    const auto sent = drain(*backend);
    REQUIRE_FALSE(sent.empty());
    const auto openLength = static_cast<std::size_t>(sent[0]);
    const auto opened = envelope::decode(std::span{sent}.subspan(1, openLength));
    REQUIRE(opened);
    const auto& open = std::get<envelope::Open>(std::get<envelope::Control>(*opened));
    CHECK(open.edition == envelope::Edition::Bedrock);
    CHECK(open.protocol == 2193);
    basalt::wire::io::ByteBuffer rest;
    rest.writeBytes(std::span{sent}.subspan(1 + openLength));
    const auto identityLength = static_cast<std::size_t>(basalt::wire::protocol::varInt::read(rest));
    const auto carried = envelope::decode(rest.readSpan(identityLength));
    REQUIRE(carried);
    CHECK(std::get<envelope::BedrockIdentity>(std::get<envelope::Control>(*carried)) == identity);
}
