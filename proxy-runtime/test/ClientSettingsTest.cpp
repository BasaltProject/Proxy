#include "RuntimeFixture.hpp"

using namespace basalt::proxy;
using namespace basalt::proxy::test;
namespace io = basalt::wire::io;

TEST_CASE("client settings are replayed in configuration form after reconfiguration acknowledgement") {
    bool malformed = false;
    bool playUpdate = false;
    bool longId = false;
    bool trailing = false;
    bool longLocale = false;
    SUBCASE("configuration settings survive repeated reconfiguration") {}
    SUBCASE("play settings replace previous configuration settings") { playUpdate = true; }
    SUBCASE("noncanonical packet id does not become part of cached payload") { longId = true; }
    SUBCASE("truncated settings close the session") { malformed = true; }
    SUBCASE("trailing settings bytes close the session") { trailing = true; }
    SUBCASE("language encoded length is bounded before retention") { longLocale = true; }
    auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto listener = io::TcpListener::bind(*address);
    REQUIRE(listener);
    REQUIRE(listener->setNonBlocking());
    auto port = listener->localPort();
    REQUIRE(port);
    auto config = settings();
    config.network.onlineMode = false;
    config.backends = {{.name = "settings", .host = "127.0.0.1", .port = *port}};
    config.defaultRoute = {"settings"};
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
    finished.insert(finished.end(), 16, 0);
    REQUIRE(backend->write(finished));
    REQUIRE(receive(client, finished.size()) == finished);
    const std::vector<std::uint8_t> ack{1, 3};
    REQUIRE(client.write(ack));
    REQUIRE(receive(*backend, ack.size()) == ack);
    // Independent vector: en_us, distance 8, full chat, colors, skin 127, right hand, no filtering/listing, all
    // particles.
    std::vector<std::uint8_t> preferences{15, 0, 5, 'e', 'n', '_', 'u', 's', 8, 0, 1, 127, 1, 0, 0, 0};
    auto transmitted = preferences;
    if (malformed) {
        transmitted.pop_back();
        --transmitted[0];
    }
    if (trailing) {
        transmitted.push_back(0);
        ++transmitted[0];
    }
    if (longLocale) {
        transmitted.erase(transmitted.begin() + 3, transmitted.begin() + 8);
        transmitted.insert(transmitted.begin() + 3, 49, static_cast<std::uint8_t>('a'));
        transmitted[2] = 49;
        transmitted[0] = static_cast<std::uint8_t>(transmitted.size() - 1);
    }
    if (longId) {
        transmitted[1] = 0x80;
        transmitted.insert(transmitted.begin() + 2, 0);
        ++transmitted[0];
    }
    REQUIRE(client.write(transmitted));
    if (malformed || trailing || longLocale) {
        CHECK(receive(client, 0, true).empty());
    } else {
        REQUIRE(receive(*backend, transmitted.size()) == transmitted);
        REQUIRE(backend->write(ack));
        REQUIRE(receive(client, ack.size()) == ack);
        REQUIRE(client.write(ack));
        REQUIRE(receive(*backend, ack.size()) == ack);
        if (playUpdate) {
            preferences[8] = 12;
            auto update = preferences;
            update[1] = 0x0e;
            REQUIRE(client.write(update));
            REQUIRE(receive(*backend, update.size()) == update);
        }
        for (unsigned cycle = 0; cycle < 2; ++cycle) {
            const std::vector<std::uint8_t> start{1, 0x78};
            REQUIRE(backend->write(start));
            REQUIRE(receive(client, start.size()) == start);
            REQUIRE(client.write(std::array<std::uint8_t, 2>{1, 0x10}));
            std::vector<std::uint8_t> expected{1, 0x10};
            expected.insert(expected.end(), preferences.begin(), preferences.end());
            CHECK(receive(*backend, expected.size()) == expected);
            REQUIRE(backend->write(ack));
            REQUIRE(receive(client, ack.size()) == ack);
            REQUIRE(client.write(ack));
            REQUIRE(receive(*backend, ack.size()) == ack);
        }
    }
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}
