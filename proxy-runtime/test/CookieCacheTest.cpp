#include "../src/CookieCache.hpp"

#include <basalt/wire/io/BudgetMemoryResource.hpp>

#include <doctest.h>

#include <array>
#include <string>

using basalt::proxy::runtime::detail::CookieCache;
namespace io = basalt::wire::io;

TEST_CASE("cookie values distinguish unknown absent and empty and remain session local") {
    io::ResourceBudget global{1024 * 1024}, local{512 * 1024};
    io::BudgetMemoryResource memory{global, local};
    {
        CookieCache cache{memory}, other{memory};
        CHECK_FALSE(cache.lookup("fixture:key").known);
        REQUIRE(cache.remember("fixture:key", std::nullopt));
        CHECK(cache.lookup("fixture:key").known);
        CHECK_FALSE(cache.lookup("fixture:key").payload);
        REQUIRE(cache.remember("fixture:key", std::span<const std::uint8_t>{}));
        REQUIRE(cache.lookup("fixture:key").payload);
        CHECK(cache.lookup("fixture:key").payload->empty());
        const std::array<std::uint8_t, 3> payload{1, 2, 3};
        REQUIRE(cache.remember("fixture:other", payload));
        REQUIRE(cache.remember("fixture:key", payload));
        REQUIRE(cache.lookup("fixture:other").payload);
        CHECK(cache.lookup("fixture:other").payload->size() == 3);
        REQUIRE(cache.lookup("fixture:key").payload);
        CHECK(cache.lookup("fixture:key").payload->back() == 3);
        CHECK_FALSE(other.lookup("fixture:key").known);
        CHECK(global.used() > 0);
    }
    CHECK(global.used() == 0);
    CHECK(local.used() == 0);
}

TEST_CASE("cookie cache bounds entries and total retained bytes without losing old values") {
    io::ResourceBudget global{1024 * 1024}, local{512 * 1024};
    io::BudgetMemoryResource memory{global, local};
    CookieCache cache{memory};
    std::array<std::uint8_t, 5120> payload{};
    payload.fill(42);
    for (unsigned i = 0; i < 12; ++i)
        REQUIRE(cache.remember("fixture:" + std::to_string(i), payload));
    CHECK_FALSE(cache.remember("fixture:overflow", payload));
    CHECK_FALSE(cache.lookup("fixture:overflow").known);
    REQUIRE(cache.lookup("fixture:0").payload);
    CHECK(cache.lookup("fixture:0").payload->size() == 5120);
    REQUIRE(cache.remember("fixture:0", std::nullopt));
    REQUIRE(cache.remember("fixture:overflow", payload));
    REQUIRE(cache.lookup("fixture:11").payload);
    CHECK(cache.lookup("fixture:11").payload->back() == 42);
    CookieCache keys{memory};
    for (unsigned i = 0; i < 32; ++i)
        REQUIRE(keys.remember("fixture:" + std::to_string(i), std::nullopt));
    CHECK_FALSE(keys.remember("fixture:33", std::nullopt));
    REQUIRE(keys.remember("fixture:0", payload));
    REQUIRE(keys.lookup("fixture:0").payload);
    CHECK(keys.lookup("fixture:0").payload->size() == 5120);
}

TEST_CASE("cookie storage checks quota before replacing the last known value") {
    io::ResourceBudget global{4096}, local{4096};
    io::BudgetMemoryResource memory{global, local};
    {
        CookieCache cache{memory};
        REQUIRE(cache.remember("fixture:key", std::nullopt));
        const auto before = global.used();
        const std::array<std::uint8_t, 5120> payload{};
        CHECK_FALSE(cache.remember("fixture:key", payload));
        CHECK(global.used() == before);
        CHECK(cache.lookup("fixture:key").known);
        CHECK_FALSE(cache.lookup("fixture:key").payload);
        CHECK_FALSE(cache.remember(std::string(257, 'x'), std::nullopt));
    }
    CHECK(global.used() == 0);
    CHECK(local.used() == 0);
}

TEST_CASE("only backend stores advance per-key revisions") {
    io::ResourceBudget global{1024 * 1024}, local{512 * 1024};
    io::BudgetMemoryResource memory{global, local};
    CookieCache cache{memory};
    const std::array<std::uint8_t, 1> payload{42};
    REQUIRE(cache.remember("fixture:key", std::nullopt));
    CHECK(cache.lookup("fixture:key").storeRevision == 0);
    REQUIRE(cache.store("fixture:key", payload));
    const auto revision = cache.lookup("fixture:key").storeRevision;
    CHECK(revision > 0);
    REQUIRE(cache.store("fixture:other", payload));
    CHECK(cache.lookup("fixture:key").storeRevision == revision);
    REQUIRE(cache.remember("fixture:key", payload));
    CHECK(cache.lookup("fixture:key").storeRevision == revision);
    REQUIRE(cache.store("fixture:key", payload));
    CHECK(cache.lookup("fixture:key").storeRevision > revision);
}
