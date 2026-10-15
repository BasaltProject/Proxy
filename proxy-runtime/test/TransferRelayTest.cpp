#include "RuntimeFixture.hpp"

using namespace basalt::proxy;
using namespace basalt::proxy::test;
namespace io = basalt::wire::io;

TEST_CASE("outgoing transfers validate the destination and drain before closing the old connection") {
    enum class Scenario { kValid, kZeroPort, kLargePort, kEmptyHost, kNulHost, kTrailing };
    auto scenario = Scenario::kValid;
    SUBCASE("valid destination") {}
    SUBCASE("zero port") { scenario = Scenario::kZeroPort; }
    SUBCASE("port outside TCP range") { scenario = Scenario::kLargePort; }
    SUBCASE("empty destination") { scenario = Scenario::kEmptyHost; }
    SUBCASE("NUL destination") { scenario = Scenario::kNulHost; }
    SUBCASE("trailing fields") { scenario = Scenario::kTrailing; }
    for (const bool play : {false, true}) {
        INFO("play = " << play);
        auto address = io::TcpAddress::literal("127.0.0.1", 0);
        REQUIRE(address);
        auto listener = io::TcpListener::bind(*address);
        REQUIRE(listener);
        REQUIRE(listener->setNonBlocking());
        auto port = listener->localPort();
        REQUIRE(port);
        auto config = settings();
        config.network.onlineMode = false;
        config.backends = {{.name = "transfer", .host = "127.0.0.1", .port = *port}};
        config.defaultRoute = {"transfer"};
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
        const std::array<std::uint8_t, 2> ack{1, 3};
        REQUIRE(client.write(ack));
        REQUIRE(receive(*backend, 2) == std::vector<std::uint8_t>{1, 3});
        if (play) {
            REQUIRE(backend->write(ack));
            REQUIRE(receive(client, 2) == std::vector<std::uint8_t>{1, 3});
            REQUIRE(client.write(ack));
            REQUIRE(receive(*backend, 2) == std::vector<std::uint8_t>{1, 3});
        }
        std::string host = "target.test";
        if (scenario == Scenario::kEmptyHost)
            host.clear();
        if (scenario == Scenario::kNulHost)
            host[2] = '\0';
        // The play packet ID 0x84 itself needs a two-byte VarInt.
        std::vector<std::uint8_t> body = play ? std::vector<std::uint8_t>{0x84, 1} : std::vector<std::uint8_t>{0x0c};
        body.push_back(static_cast<std::uint8_t>(host.size()));
        body.insert(body.end(), host.begin(), host.end());
        if (scenario == Scenario::kZeroPort)
            body.push_back(0);
        else if (scenario == Scenario::kLargePort)
            body.insert(body.end(), {0x80, 0x80, 4});
        else
            body.insert(body.end(), {0xdd, 0xc7, 1});
        if (scenario == Scenario::kTrailing)
            body.push_back(0);
        std::vector<std::uint8_t> frame{static_cast<std::uint8_t>(body.size())};
        frame.insert(frame.end(), body.begin(), body.end());
        REQUIRE(backend->write(frame));
        backend->close();
        const auto response = receive(client, 0, true);
        if (scenario == Scenario::kValid)
            CHECK(response == frame);
        else
            CHECK(response.empty());
        proxy.stop();
        CHECK(proxy.runtime.sessionCount() == 0);
        CHECK(proxy.runtime.bufferedBytes() == 0);
    }
}
