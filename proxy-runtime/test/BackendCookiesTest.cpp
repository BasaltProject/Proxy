#include "../src/BackendLogin.hpp"
#include "../src/CookieCache.hpp"
#include "../src/PendingCookies.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/io/BudgetMemoryResource.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>

#include <doctest.h>

#include <array>
#include <limits>
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
        const auto initialPending = first.pendingBytes();
        CHECK_FALSE(first.answerCookie(
            cookie::ResponsePacket{"fixture:first", std::vector<std::uint8_t>(cookie::kMaxPayloadBytes + 1, 0)},
            cache));
        CHECK(first.hasCookieRequest());
        CHECK(first.pendingBytes() == initialPending);
        CHECK_FALSE(cache.lookup("fixture:first").known);
        REQUIRE(first.answerCookie(cookie::ResponsePacket{"fixture:first", maximum}, cache));
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
        const auto pendingBeforeInvalidRelay = first.pendingBytes();
        CHECK_FALSE(first.relayCookie({"fixture:first", std::nullopt}));
        CHECK(first.pendingBytes() == pendingBeforeInvalidRelay);
        CHECK(local.used() >= first.allocated() + second.allocated() + cache.allocated());
    }
    CHECK(local.used() == 0);
    CHECK(global.used() == 0);
}

TEST_CASE("pending cookie correlation retains exact backend generation through compaction") {
    io::ResourceBudget global{64 * 1024};
    io::ResourceBudget local{64 * 1024};
    io::BudgetMemoryResource memory{global, local};
    {
        PendingCookies pending{memory};
        REQUIRE(pending.add("fixture:key", 11, {1, 0}));
        REQUIRE(pending.add("fixture:other", 22, {2, 1}));
        REQUIRE(pending.add("fixture:key", 33, {3, 0}));
        const auto other = pending.consume("fixture:other");
        REQUIRE(other);
        CHECK(*other == PendingCookie{22, {2, 1}});
        const auto first = pending.consume("fixture:key");
        REQUIRE(first);
        CHECK(*first == PendingCookie{11, {1, 0}});
        const auto revisit = pending.consume("fixture:key");
        REQUIRE(revisit);
        CHECK(*revisit == PendingCookie{33, {3, 0}});
        CHECK_FALSE(pending.consume("fixture:key"));
        CHECK_FALSE(pending.consume("fixture:missing"));
        CHECK_FALSE(pending.add("fixture:key", 0, {0, 0}));
        CHECK_FALSE(pending.add("fixture:key", 0, {1, 256}));
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        REQUIRE(pending.add("fixture:maximum", maximum, {maximum, 255}));
        const auto restored = pending.consume("fixture:maximum");
        REQUIRE(restored);
        CHECK(*restored == PendingCookie{maximum, {maximum, 255}});
        const std::string key = "fixture:" + std::string(248, 'a');
        REQUIRE(key.size() == 256);
        for (std::uint64_t i = 0; i < 32; ++i)
            REQUIRE(pending.add(key, i, {i + 1, static_cast<std::size_t>(i)}));
        CHECK_FALSE(pending.add(key, 32, {33, 0}));
        for (std::uint64_t i = 0; i < 32; ++i) {
            const auto request = pending.consume(key);
            REQUIRE(request);
            CHECK(*request == PendingCookie{i, {i + 1, static_cast<std::size_t>(i)}});
        }
        CHECK_FALSE(pending.consume(key));
        CHECK_FALSE(pending.add("", 0, {1, 0}));
        CHECK_FALSE(pending.add(key + "a", 0, {1, 0}));
    }
    CHECK(global.used() == 0);
    CHECK(local.used() == 0);
}

TEST_CASE("failed cookie-origin reservation preserves all pending requests") {
    io::ResourceBudget global{4096};
    io::ResourceBudget local{4096};
    io::BudgetMemoryResource memory{global, local};
    {
        PendingCookies pending{memory};
        const std::string key = "fixture:" + std::string(248, 'a');
        std::size_t count = 0;
        while (count < 32) {
            const auto reserved = local.used();
            if (!pending.add(key, count, {count + 1, count})) {
                CHECK(local.used() == reserved);
                break;
            }
            ++count;
        }
        REQUIRE(count > 0);
        REQUIRE(count < 32);
        for (std::size_t i = 0; i < count; ++i) {
            const auto request = pending.consume(key);
            REQUIRE(request);
            CHECK(*request == PendingCookie{i, {i + 1, i}});
        }
        CHECK_FALSE(pending.consume(key));
    }
    CHECK(global.used() == 0);
    CHECK(local.used() == 0);
}
