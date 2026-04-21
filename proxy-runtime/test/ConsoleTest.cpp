#include "../src/Console.hpp"

#include "RuntimeFixture.hpp"

using namespace basalt::proxy;
using namespace basalt::proxy::test;

TEST_CASE("console dispatch validates arity and sends owner requests without client command interception") {
    auto config = settings();
    config.backends = {{.name = "lobby", .host = "127.0.0.1", .port = 1}};
    config.defaultRoute = {"lobby"};
    Running proxy{config};
    const auto execute = [&](std::string_view line) {
        return runtime::detail::consoleCommand(line, proxy.runtime, config.backends, {});
    };
    CHECK(execute("  /servers\t").message == "Backends: lobby");
    CHECK(execute(" /stop ").stop);
    CHECK_FALSE(execute("stop now").stop);
    CHECK(execute(" ").message.empty());
    CHECK(execute("send Nobody").message.starts_with("Commands:"));
    CHECK(execute("send Nobody lobby extra").message.starts_with("Commands:"));
    CHECK(execute(std::string(4097, 'a')).message == "Invalid console command.");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    auto absent = execute("send Nobody lobby");
    while (absent.message == "Proxy is not running." && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
        absent = execute("send Nobody lobby");
    }
    CHECK(absent.message.starts_with("Player not found"));
    CHECK(execute("send Nobody missing").message == "Unknown backend.");
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
}
