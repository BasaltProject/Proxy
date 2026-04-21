#include "../src/ResourcePacks.hpp"

#include <basalt/wire/io/BudgetMemoryResource.hpp>

#include <doctest.h>

#include <array>
#include <limits>

using namespace basalt::proxy::runtime::detail;
namespace io = basalt::wire::io;
using basalt::wire::common::Uuid;

TEST_CASE("pack IDs isolate backend generations and individual removals") {
    io::ResourceBudget global{8192}, local{8192};
    io::BudgetMemoryResource memory{global, local};
    {
        ResourcePacks packs{memory};
        const Uuid original{0, 42};
        const BackendAttempt first{1, 0};
        const auto published = packs.publish(original, first);
        REQUIRE(published);
        CHECK(packs.publish(original, first) == published);
        REQUIRE(packs.origin(*published));
        CHECK(packs.origin(*published)->id == original);
        CHECK(packs.origin(*published)->backend == first);
        CHECK_FALSE(packs.origin(original));
        CHECK_FALSE(packs.remove(original, {2, 0}));
        CHECK(packs.remove(original, first) == published);
        CHECK_FALSE(packs.origin(*published));
        const auto replacement = packs.publish(original, first);
        REQUIRE(replacement);
        CHECK(replacement != published);
        packs.clear();
        const auto returned = packs.publish(original, {3, 0});
        REQUIRE(returned);
        CHECK(returned != replacement);
        CHECK(returned != published);
        CHECK_FALSE(packs.origin(*replacement));
        CHECK_FALSE(packs.publish(original, {0, 0}));
        CHECK_FALSE(packs.publish(original, {4, 256}));
        CHECK(packs.allocated() <= local.used());
    }
    CHECK(global.used() == 0);
}

TEST_CASE("pack correlation is bounded and recovers capacity without recycling public IDs") {
    io::ResourceBudget global{8192}, local{8192};
    io::BudgetMemoryResource memory{global, local};
    ResourcePacks packs{memory};
    const BackendAttempt attempt{std::numeric_limits<std::uint64_t>::max(), 255};
    std::array<Uuid, 32> published{};
    for (std::size_t i = 0; i < published.size(); ++i) {
        const auto id = packs.publish({0, i}, attempt);
        REQUIRE(id);
        published[i] = *id;
    }
    CHECK_FALSE(packs.publish({0, 32}, attempt));
    CHECK(packs.publish({0, 31}, attempt) == published[31]);
    CHECK(packs.remove({0, 12}, attempt) == published[12]);
    CHECK_FALSE(packs.origin(published[12]));
    const auto replacement = packs.publish({0, 32}, attempt);
    REQUIRE(replacement);
    CHECK(*replacement != published[12]);
    for (std::size_t i = 0; i < published.size(); ++i) {
        if (i == 12)
            continue;
        REQUIRE(packs.origin(published[i]));
        CHECK(packs.origin(published[i])->id == Uuid{0, i});
        CHECK(packs.origin(published[i])->backend == attempt);
    }
    packs.clear();
    for (const auto id : published)
        CHECK_FALSE(packs.origin(id));
}

TEST_CASE("pack publication fails without leaking or consuming existing mappings") {
    io::ResourceBudget global{64}, local{64};
    io::BudgetMemoryResource memory{global, local};
    {
        ResourcePacks packs{memory};
        const auto first = packs.publish({0, 1}, {1, 0});
        REQUIRE(first);
        CHECK_FALSE(packs.publish({0, 2}, {1, 0}));
        REQUIRE(packs.origin(*first));
        CHECK(packs.origin(*first)->id == Uuid{0, 1});
        packs.clear();
        CHECK(packs.publish({0, 2}, {1, 0}));
    }
    CHECK(global.used() == 0);
}
