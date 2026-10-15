#include "../src/BackendLogin.hpp"
#include "../src/CookieCache.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/io/BudgetMemoryResource.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>

#include <doctest.h>

#include <array>
#include <utility>
#include <vector>

using namespace basalt::proxy;
using namespace runtime::detail;
namespace io = basalt::wire::io;
namespace cookie = basalt::wire::protocol::cookie;

TEST_CASE("backend login cookie requests are isolated and late replies preserve newer stores") {
    io::ResourceBudget global{256 * 1024};
    io::ResourceBudget local{256 * 1024};
    io::BudgetMemoryResource memory{global, local};
    {
        CookieCache cache{memory};
        BackendLogin first{io::TcpConnection{}, memory, {1, 0}};
        BackendLogin second{io::TcpConnection{}, memory, {2, 1}};
        basalt::wire::auth::SessionProfile profile;
        profile.name = "Tester";
        profile.id = basalt::wire::common::Uuid::offlinePlayer(profile.name);
        config::BackendConfig config;
        REQUIRE(first.start(config, profile, "localhost", 25565, "127.0.0.1"));
        REQUIRE(second.start(config, profile, "localhost", 25565, "127.0.0.1"));
        const auto request = [&](BackendLogin& endpoint, std::string key) {
            io::ByteBuffer packet{512};
            cookie::RequestPacket{std::move(key)}.write(packet);
            return endpoint.requestCookie(packet, cache);
        };
        const auto answer = [&](BackendLogin& endpoint, cookie::ResponsePacket reply) {
            io::ByteBuffer packet{cookie::kMaxPayloadBytes + 512};
            reply.write(packet);
            return endpoint.answerCookie(packet, cache);
        };
        REQUIRE(request(first, "fixture:first") == BackendLogin::CookieAction::kRequestClient);
        REQUIRE(request(second, "fixture:second") == BackendLogin::CookieAction::kRequestClient);
        CHECK_FALSE(request(first, "fixture:duplicate"));
        CHECK_FALSE(answer(second, {"fixture:first", std::nullopt}));
        CHECK(first.hasCookieRequest());
        CHECK_FALSE(first.interest().readable);
        CHECK(second.hasCookieRequest());
        const std::vector<std::uint8_t> maximum(cookie::kMaxPayloadBytes, 7);
        REQUIRE(answer(first, {"fixture:first", maximum}));
        CHECK_FALSE(first.hasCookieRequest());
        CHECK(first.interest().readable);
        CHECK(second.hasCookieRequest());
        REQUIRE(cache.lookup("fixture:first").payload);
        CHECK(cache.lookup("fixture:first").payload->size() == maximum.size());
        REQUIRE(answer(second, {"fixture:second", std::nullopt}));
        CHECK(cache.lookup("fixture:second").known);
        CHECK_FALSE(cache.lookup("fixture:second").payload);
        CHECK_FALSE(second.hasCookieRequest());
        REQUIRE(request(first, "fixture:second") == BackendLogin::CookieAction::kCached);
        CHECK_FALSE(first.hasCookieRequest());
        CHECK_FALSE(answer(first, {"fixture:second", std::nullopt}));
        REQUIRE(request(first, "fixture:race") == BackendLogin::CookieAction::kRequestClient);
        REQUIRE(request(second, "fixture:race") == BackendLogin::CookieAction::kRequestClient);
        const std::array<std::uint8_t, 1> stored{9};
        REQUIRE(cache.store("fixture:race", stored));
        REQUIRE(answer(first, {"fixture:race", std::nullopt}));
        REQUIRE(answer(second, {"fixture:race", maximum}));
        const auto latest = cache.lookup("fixture:race");
        REQUIRE(latest.payload);
        REQUIRE(latest.payload->size() == 1);
        CHECK((*latest.payload)[0] == 9);
        REQUIRE(request(first, "fixture:empty") == BackendLogin::CookieAction::kRequestClient);
        REQUIRE(answer(first, {"fixture:empty", std::vector<std::uint8_t>{}}));
        const auto empty = cache.lookup("fixture:empty");
        REQUIRE(empty.known);
        REQUIRE(empty.payload);
        CHECK(empty.payload->empty());
        CHECK(local.used() >= first.allocated() + second.allocated() + cache.allocated());
    }
    CHECK(local.used() == 0);
    CHECK(global.used() == 0);
}
