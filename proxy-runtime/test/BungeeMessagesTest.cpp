#include "RuntimeFixture.hpp"

using namespace basalt::proxy;
using namespace basalt::proxy::test;

namespace {
std::vector<std::uint8_t> pluginFrame(std::uint8_t id, std::string_view command, std::string_view value = {},
                                      std::string_view channel = "bungeecord:main") {
    std::vector<std::uint8_t> body{id, static_cast<std::uint8_t>(channel.size())};
    body.insert(body.end(), channel.begin(), channel.end());
    body.push_back(static_cast<std::uint8_t>(command.size() >> 8));
    body.push_back(static_cast<std::uint8_t>(command.size()));
    body.insert(body.end(), command.begin(), command.end());
    if (!value.empty()) {
        body.push_back(static_cast<std::uint8_t>(value.size() >> 8));
        body.push_back(static_cast<std::uint8_t>(value.size()));
        body.insert(body.end(), value.begin(), value.end());
    }
    std::vector<std::uint8_t> result;
    auto size = body.size();
    do {
        const auto byte = static_cast<std::uint8_t>(size & 127);
        size >>= 7;
        result.push_back(static_cast<std::uint8_t>(byte | (size ? 128 : 0)));
    } while (size);
    result.insert(result.end(), body.begin(), body.end());
    return result;
}
} // namespace

TEST_CASE("Bungee queries require backend opt-in and cannot acquire authority from the client") {
    enum class Scenario {
        kServer,
        kRouted,
        kLegacyServer,
        kServers,
        kLargeServers,
        kUuid,
        kOnlineUuid,
        kDisabled,
        kUnknown,
        kMalformed,
        kLateConfiguration,
        kClient,
        kLegacyClient
    };
    auto scenario = Scenario::kServer;
    SUBCASE("GetServer") {}
    SUBCASE("permission and name belong to the selected backend") { scenario = Scenario::kRouted; }
    SUBCASE("legacy backend alias normalizes to the native channel") { scenario = Scenario::kLegacyServer; }
    SUBCASE("GetServers") { scenario = Scenario::kServers; }
    SUBCASE("maximum configured backend list exceeds bootstrap safely") { scenario = Scenario::kLargeServers; }
    SUBCASE("UUID uses verified undashed identity") { scenario = Scenario::kUuid; }
    SUBCASE("UUID retains verified online identity through an offline backend") { scenario = Scenario::kOnlineUuid; }
    SUBCASE("disabled backend messages are consumed") { scenario = Scenario::kDisabled; }
    SUBCASE("unsupported administrative subcommand is consumed") { scenario = Scenario::kUnknown; }
    SUBCASE("known query with trailing data is rejected") { scenario = Scenario::kMalformed; }
    SUBCASE("control packets after configuration finish are rejected") { scenario = Scenario::kLateConfiguration; }
    SUBCASE("client control messages are discarded") { scenario = Scenario::kClient; }
    SUBCASE("legacy client control alias is also discarded") { scenario = Scenario::kLegacyClient; }
    const auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    const auto port = listener->localPort();
    REQUIRE(port);
    auto config = settings();
    const bool online = scenario == Scenario::kOnlineUuid;
    config.network.onlineMode = online;
    config.backends = {{.name = "lobby", .host = "127.0.0.1", .port = *port},
                       {.name = "game", .host = "127.0.0.1", .port = *port}};
    if (scenario == Scenario::kLargeServers) {
        for (unsigned i = 0; i < 254; ++i) {
            auto name = std::to_string(i);
            name.append(64 - name.size(), 'a');
            config.backends.push_back({.name = std::move(name), .host = "127.0.0.1", .port = *port});
        }
    }
    std::string serverList;
    for (const auto& server : config.backends) {
        if (!serverList.empty())
            serverList += ", ";
        serverList += server.name;
    }
    config.backends.front().bungeeMessages = scenario != Scenario::kDisabled;
    config.defaultRoute = {"lobby"};
    if (scenario == Scenario::kRouted) {
        config.defaultRoute = {"game"};
        config.backends[0].bungeeMessages = false;
        config.backends[1].bungeeMessages = true;
    }
    runtime::RuntimeServices services;
    services.authentication = [](auto, auto, auto) {
        const std::string json = "{\"id\":\"0123456789abcdef0123456789abcdef\",\"name\":\"Tester\"}";
        return std::expected<basalt::wire::auth::HttpsResponse, basalt::wire::auth::HttpsError>{
            basalt::wire::auth::HttpsResponse{200, {json.begin(), json.end()}}};
    };
    Running proxy{std::move(config), std::move(services)};
    auto client = proxy.connect();
    loginStart(client, proxy.runtime.port());
    std::optional<basalt::wire::auth::Cfb8Cipher> encryptor;
    std::optional<basalt::wire::auth::Cfb8Cipher> decryptor;
    if (online) {
        const auto secret = keyResponse(client);
        auto enc = basalt::wire::auth::Cfb8Cipher::create(secret);
        auto dec = basalt::wire::auth::Cfb8Cipher::create(secret);
        REQUIRE(enc);
        REQUIRE(dec);
        encryptor = std::move(*enc);
        decryptor = std::move(*dec);
    }
    const auto clientRead = [&](std::size_t minimum, bool eof = false) {
        auto bytes = receive(client, minimum, eof);
        if (decryptor)
            decryptor->decrypt(bytes);
        return bytes;
    };
    const auto clientWrite = [&](std::vector<std::uint8_t> bytes) {
        if (encryptor)
            encryptor->encrypt(bytes);
        return client.write(bytes);
    };
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
    auto frontendFinished = finished;
    if (online) {
        const std::array<std::uint8_t, 16> identity{1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                                    1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
        std::copy(identity.begin(), identity.end(), frontendFinished.begin() + 2);
    }
    REQUIRE(clientRead(frontendFinished.size()) == frontendFinished);
    const std::vector<std::uint8_t> ack{1, 3};
    REQUIRE(clientWrite(ack));
    REQUIRE(receive(*backend, ack.size()) == ack);
    REQUIRE(backend->write(ack));
    REQUIRE(clientRead(ack.size()) == ack);
    if (scenario == Scenario::kLateConfiguration) {
        REQUIRE(backend->write(pluginFrame(0x01, "GetServer")));
        CHECK(clientRead(0, true).empty());
        proxy.stop();
        CHECK(proxy.runtime.sessionCount() == 0);
        CHECK(proxy.runtime.bufferedBytes() == 0);
        return;
    }
    REQUIRE(clientWrite(ack));
    REQUIRE(receive(*backend, ack.size()) == ack);
    std::string command = "GetServer";
    std::string value = scenario == Scenario::kRouted ? "game" : "lobby";
    if (scenario == Scenario::kServers || scenario == Scenario::kLargeServers) {
        command = "GetServers";
        value = serverList;
    } else if (scenario == Scenario::kUuid || online) {
        command = "UUID";
        value = online ? "0123456789abcdef0123456789abcdef" : "f3d28cb072253cb1baeb2dadd2be89ae";
    } else if (scenario == Scenario::kUnknown) {
        command = "ConnectOther";
    }
    const std::vector<std::uint8_t> keepalive{9, 0x2d, 0, 0, 0, 0, 0, 0, 0, 99};
    if (scenario == Scenario::kClient || scenario == Scenario::kLegacyClient) {
        REQUIRE(backend->write(keepalive));
        REQUIRE(clientRead(keepalive.size()) == keepalive);
        auto spoof = pluginFrame(0x16, "ConnectOther", "Tester",
                                 scenario == Scenario::kLegacyClient ? "BungeeCord" : "bungeecord:main");
        spoof.insert(spoof.end(), {0, 4, 'g', 'a', 'm', 'e'});
        spoof[0] += 6;
        auto response = keepalive;
        response[1] = 0x1c;
        spoof.insert(spoof.end(), response.begin(), response.end());
        REQUIRE(clientWrite(spoof));
        CHECK(receive(*backend, response.size()) == response);
        auto marker = keepalive;
        marker.back() = 100;
        REQUIRE(backend->write(marker));
        CHECK(clientRead(marker.size()) == marker);
    } else {
        auto request =
            pluginFrame(0x18, command, {}, scenario == Scenario::kLegacyServer ? "BungeeCord" : "bungeecord:main");
        if (scenario == Scenario::kMalformed) {
            request.push_back(0);
            ++request[0];
        } else {
            request.insert(request.end(), keepalive.begin(), keepalive.end());
        }
        REQUIRE(backend->write(request));
        if (scenario == Scenario::kMalformed) {
            CHECK(clientRead(0, true).empty());
        } else {
            REQUIRE(clientRead(keepalive.size()) == keepalive);
            if (scenario != Scenario::kDisabled && scenario != Scenario::kUnknown) {
                const auto expected = pluginFrame(0x16, command, value);
                CHECK(receive(*backend, expected.size()) == expected);
            }
            auto response = keepalive;
            response[1] = 0x1c;
            REQUIRE(clientWrite(response));
            CHECK(receive(*backend, response.size()) == response);
        }
    }
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}
