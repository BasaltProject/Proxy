#include "RuntimeFixture.hpp"

using namespace basalt::proxy;
using namespace basalt::proxy::test;
namespace io = basalt::wire::io;

namespace {
std::vector<std::uint8_t> cookieFrame(std::uint8_t id, std::string_view key,
                                      std::span<const std::uint8_t> suffix = {}) {
    std::vector<std::uint8_t> body{id, static_cast<std::uint8_t>(key.size())};
    body.insert(body.end(), key.begin(), key.end());
    body.insert(body.end(), suffix.begin(), suffix.end());
    std::vector<std::uint8_t> frame;
    auto size = body.size();
    do {
        auto byte = static_cast<std::uint8_t>(size & 127);
        size >>= 7;
        frame.push_back(static_cast<std::uint8_t>(byte | (size ? 128 : 0)));
    } while (size);
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}
} // namespace

TEST_CASE("configuration and play cookies are bounded and correlated before relay") {
    enum class Scenario { kValid, kWrongKey, kUnsolicited, kMalformedStore, kTooMany, kMaximum, kReplay, kCacheLimit };
    auto scenario = Scenario::kValid;
    SUBCASE("multiple outstanding requests accept responses in reverse order") {}
    SUBCASE("response with unrelated key is rejected") { scenario = Scenario::kWrongKey; }
    SUBCASE("unsolicited response is rejected") { scenario = Scenario::kUnsolicited; }
    SUBCASE("oversized store declaration is rejected") { scenario = Scenario::kMalformedStore; }
    SUBCASE("outstanding requests have a fixed limit") { scenario = Scenario::kTooMany; }
    SUBCASE("maximum cookie response and store pass") { scenario = Scenario::kMaximum; }
    SUBCASE("a consumed request cannot authorize a replayed response") { scenario = Scenario::kReplay; }
    SUBCASE("stored values cannot exceed the cache byte ceiling") { scenario = Scenario::kCacheLimit; }
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
        const auto requestId = static_cast<std::uint8_t>(play ? 0x15 : 0);
        const auto responseId = static_cast<std::uint8_t>(play ? 0x15 : 1);
        const auto storeId = static_cast<std::uint8_t>(play ? 0x7a : 0x0b);
        const auto request = cookieFrame(requestId, "fixture:test");
        const std::array<std::uint8_t, 1> absent{0};
        if (scenario == Scenario::kUnsolicited) {
            REQUIRE(client.write(cookieFrame(responseId, "fixture:test", absent)));
            CHECK(receive(client, 0, true).empty());
        } else if (scenario == Scenario::kMalformedStore) {
            const std::array<std::uint8_t, 2> oversized{0x81, 0x28};
            REQUIRE(backend->write(cookieFrame(storeId, "fixture:test", oversized)));
            CHECK(receive(client, 0, true).empty());
        } else if (scenario == Scenario::kCacheLimit) {
            const auto oldRequest = cookieFrame(requestId, "fixture:stored0");
            REQUIRE(backend->write(oldRequest));
            REQUIRE(receive(client, oldRequest.size()) == oldRequest);
            std::vector<std::uint8_t> payload{0x80, 0x28};
            payload.insert(payload.end(), 5120, 0x42);
            for (unsigned i = 0; i < 12; ++i) {
                const auto store = cookieFrame(storeId, "fixture:stored" + std::to_string(i), payload);
                REQUIRE(backend->write(store));
                CHECK(receive(client, store.size()) == store);
            }
            // This reply was computed before Store; it must not erase the newer cached value.
            const auto staleResponse = cookieFrame(responseId, "fixture:stored0", absent);
            REQUIRE(client.write(staleResponse));
            CHECK(receive(*backend, staleResponse.size()) == staleResponse);
            REQUIRE(backend->write(cookieFrame(storeId, "fixture:overflow", payload)));
            CHECK(receive(client, 0, true).empty());
        } else if (scenario == Scenario::kTooMany) {
            for (unsigned i = 0; i < 32; ++i) {
                REQUIRE(backend->write(request));
                REQUIRE(receive(client, request.size()) == request);
            }
            REQUIRE(backend->write(request));
            CHECK(receive(client, 0, true).empty());
        } else {
            REQUIRE(backend->write(request));
            REQUIRE(receive(client, request.size()) == request);
            if (scenario == Scenario::kWrongKey) {
                REQUIRE(client.write(cookieFrame(responseId, "fixture:wrong", absent)));
                CHECK(receive(client, 0, true).empty());
            } else if (scenario == Scenario::kMaximum) {
                std::vector<std::uint8_t> payload{1, 0x80, 0x28};
                payload.insert(payload.end(), 5120, 0x42);
                const auto response = cookieFrame(responseId, "fixture:test", payload);
                REQUIRE(client.write(response));
                CHECK(receive(*backend, response.size()) == response);
                payload.erase(payload.begin());
                const auto store = cookieFrame(storeId, "fixture:test", payload);
                REQUIRE(backend->write(store));
                CHECK(receive(client, store.size()) == store);
            } else {
                const auto other = cookieFrame(requestId, "fixture:other");
                REQUIRE(backend->write(other));
                REQUIRE(receive(client, other.size()) == other);
                const auto last = cookieFrame(requestId, "fixture:last");
                REQUIRE(backend->write(last));
                REQUIRE(receive(client, last.size()) == last);
                for (const auto key : {"fixture:other", "fixture:test", "fixture:last"}) {
                    const auto response = cookieFrame(responseId, key, absent);
                    REQUIRE(client.write(response));
                    CHECK(receive(*backend, response.size()) == response);
                }
                if (scenario == Scenario::kReplay) {
                    REQUIRE(client.write(cookieFrame(responseId, "fixture:test", absent)));
                    CHECK(receive(client, 0, true).empty());
                }
            }
        }
        proxy.stop();
        CHECK(proxy.runtime.sessionCount() == 0);
        CHECK(proxy.runtime.bufferedBytes() == 0);
    }
}
