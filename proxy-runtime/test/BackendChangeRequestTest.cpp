#include "RuntimeFixture.hpp"

using namespace basalt::proxy;
using namespace basalt::proxy::test;

namespace {
auto submitWhenRunning(runtime::ProxyRuntime& proxy) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    auto submitted = proxy.sendPlayer("Nobody", "lobby");
    while (!submitted &&
           (submitted.error() == runtime::BackendChangeError::kNotRunning ||
            submitted.error() == runtime::BackendChangeError::kBusy) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
        submitted = proxy.sendPlayer("Nobody", "lobby");
    }
    return submitted;
}

config::ProxyConfig configuration() {
    auto config = settings();
    config.runtime.ioLoops = 1;
    config.runtime.maxPendingWork = 1;
    config.backends = {{.name = "lobby", .host = "127.0.0.1", .port = 1}};
    config.defaultRoute = {"lobby"};
    return config;
}
} // namespace

TEST_CASE("control requests reject invalid admission and retain bounded publication resources") {
    auto stopped = makeRuntime(configuration());
    auto beforeRun = stopped.sendPlayer("Nobody", "lobby");
    REQUIRE_FALSE(beforeRun);
    CHECK(beforeRun.error() == runtime::BackendChangeError::kNotRunning);
    Running proxy{configuration()};
    {
        auto first = submitWhenRunning(proxy.runtime);
        REQUIRE(first);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!first->result() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        REQUIRE(first->result());
        CHECK(*first->result() == runtime::BackendChangeResult::kPlayerNotFound);
        const auto occupied = proxy.runtime.bufferedBytes();
        const auto second = proxy.runtime.sendPlayer("Nobody", "lobby");
        REQUIRE_FALSE(second);
        CHECK(second.error() == runtime::BackendChangeError::kBusy);
        CHECK(proxy.runtime.bufferedBytes() == occupied);
        const auto invalid = proxy.runtime.sendPlayer("bad name", "lobby");
        REQUIRE_FALSE(invalid);
        CHECK(invalid.error() == runtime::BackendChangeError::kInvalidPlayer);
        const auto unknown = proxy.runtime.sendPlayer("Nobody", "missing");
        REQUIRE_FALSE(unknown);
        CHECK(unknown.error() == runtime::BackendChangeError::kUnknownBackend);
    }
    auto retried = submitWhenRunning(proxy.runtime);
    REQUIRE(retried);
    retried = std::unexpected(runtime::BackendChangeError::kBusy);
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
    const auto afterStop = proxy.runtime.sendPlayer("Nobody", "lobby");
    REQUIRE_FALSE(afterStop);
    CHECK(afterStop.error() == runtime::BackendChangeError::kNotRunning);
}

TEST_CASE("control ticket can be observed after its runtime has stopped and been destroyed") {
    std::optional<runtime::BackendChangeRequest> ticket;
    {
        Running proxy{configuration()};
        auto submitted = submitWhenRunning(proxy.runtime);
        REQUIRE(submitted);
        ticket = std::move(*submitted);
        proxy.stop();
    }
    REQUIRE(ticket);
    REQUIRE(ticket->result());
    CHECK((*ticket->result() == runtime::BackendChangeResult::kStopped ||
           *ticket->result() == runtime::BackendChangeResult::kPlayerNotFound));
}
