#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "RuntimeFixture.hpp"

#include <doctest.h>

#include <string_view>

using namespace basalt::proxy;
using namespace basalt::proxy::test;

namespace {
std::vector<std::uint8_t> bytesOf(std::string_view text) { return {text.begin(), text.end()}; }
} // namespace

TEST_CASE("a trusted upstream's PROXY header is read and anyone else's is not believed") {
    auto config = settings();
    config.network.proxyProtocol = true;
    config.network.proxyProtocolTrusted = {"127.0.0.0/8"};
    {
        Running server{config};
        auto client = server.connect();
        const auto header = bytesOf("PROXY TCP4 203.0.113.7 10.0.0.1 5555 25565\r\n");
        REQUIRE(client.writeSome(header));
        const auto hello = handshake(server.runtime.port());
        REQUIRE(client.writeSome(hello));
        const std::array<std::uint8_t, 2> request{1, 0};
        REQUIRE(client.writeSome(request));
        const auto response = receive(client, 20);
        CHECK(std::string_view{reinterpret_cast<const char*>(response.data()), response.size()}.find(
                  "\"protocol\":777") != std::string_view::npos);
    }
    config.network.proxyProtocolTrusted = {"203.0.113.0/24"};
    Running server{config};
    auto client = server.connect();
    const auto header = bytesOf("PROXY TCP4 203.0.113.7 10.0.0.1 5555 25565\r\n");
    REQUIRE(client.writeSome(header));
    CHECK(receive(client, 1, true).empty());
}
