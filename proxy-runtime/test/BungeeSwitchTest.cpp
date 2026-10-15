#include "../src/Console.hpp"
#include "RuntimeFixture.hpp"

using namespace basalt::proxy;
using namespace basalt::proxy::test;

namespace {
std::vector<std::uint8_t> connectMessage(std::string_view target, std::string_view channel = "bungeecord:main") {
    std::vector<std::uint8_t> packet{0, 0x18, static_cast<std::uint8_t>(channel.size())};
    packet.insert(packet.end(), channel.begin(), channel.end());
    packet.insert(packet.end(), {0, 7, 'C', 'o', 'n', 'n', 'e', 'c', 't', 0, static_cast<std::uint8_t>(target.size())});
    packet.insert(packet.end(), target.begin(), target.end());
    packet[0] = static_cast<std::uint8_t>(packet.size() - 1);
    return packet;
}

std::vector<std::uint8_t> loginSuccess() {
    std::vector<std::uint8_t> result{41,   2,    0xf3, 0xd2, 0x8c, 0xb0, 0x72, 0x25, 0x3c, 0xb1, 0xba, 0xeb, 0x2d,
                                     0xad, 0xd2, 0xbe, 0x89, 0xae, 6,    'T',  'e',  's',  't',  'e',  'r',  0};
    result.insert(result.end(), 16, 0);
    return result;
}

io::TcpConnection acceptBackend(io::TcpListener& listener) {
    io::EventLoop loop;
    REQUIRE(loop.watch(listener.nativeHandle(), {true, false}, 1));
    const auto ready = loop.wait(2000);
    REQUIRE(ready);
    REQUIRE_FALSE(ready->empty());
    auto accepted = listener.accept();
    REQUIRE(accepted);
    REQUIRE(accepted->setNonBlocking());
    return std::move(*accepted);
}
} // namespace

TEST_CASE("backend change requests drive runtime ownership without public reconnect") {
    enum class Scenario {
        kCycle,
        kLegacy,
        kDisabled,
        kMalformed,
        kCandidateFailure,
        kCandidateTimeout,
        kConsole,
        kConsoleCommand
    };
    auto scenario = Scenario::kCycle;
    SUBCASE("A B A through owners DNS workers and timers") {}
    SUBCASE("legacy channel requests the same native change") { scenario = Scenario::kLegacy; }
    SUBCASE("backend must opt in") { scenario = Scenario::kDisabled; }
    SUBCASE("Connect with trailing data closes before dialing") { scenario = Scenario::kMalformed; }
    SUBCASE("candidate identity failure preserves active and allows retry") { scenario = Scenario::kCandidateFailure; }
    SUBCASE("candidate preparation expires through runtime timer and permits retry") {
        scenario = Scenario::kCandidateTimeout;
    }
    SUBCASE("console request changes backend without Bungee permission") { scenario = Scenario::kConsole; }
    SUBCASE("console send dispatcher requests repeated changes") { scenario = Scenario::kConsoleCommand; }
    const auto address = io::TcpAddress::literal("127.0.0.1", 0);
    REQUIRE(address);
    auto config = settings();
    config.network.onlineMode = false;
    if (scenario == Scenario::kConsole || scenario == Scenario::kConsoleCommand)
        config.runtime.maxPendingWork = 1;
    config.runtime.loginTimeoutMs = scenario == Scenario::kCandidateTimeout ? 1000 : 3000;
    std::array<io::TcpListener, 2> listeners;
    for (std::size_t i = 0; i < listeners.size(); ++i) {
        auto listener = io::TcpListener::bind(*address);
        REQUIRE(listener);
        REQUIRE(listener->setNonBlocking());
        const auto port = listener->localPort();
        REQUIRE(port);
        config.backends.push_back({.name = i == 0 ? "a" : "b", .host = "127.0.0.1", .port = *port});
        config.backends.back().bungeeMessages =
            scenario != Scenario::kDisabled && scenario != Scenario::kConsole && scenario != Scenario::kConsoleCommand;
        listeners[i] = std::move(*listener);
    }
    config.defaultRoute = {"a"};
    const auto commandBackends = config.backends;
    Running proxy{std::move(config)};
    auto client = proxy.connect();
    const auto publicHandle = client.nativeHandle();
    loginStart(client, proxy.runtime.port());
    auto active = acceptBackend(listeners[0]);
    REQUIRE(receive(active, 42).size() == 42);
    const auto finished = loginSuccess();
    REQUIRE(active.write(finished));
    REQUIRE(receive(client, finished.size()) == finished);
    const std::vector<std::uint8_t> finish{1, 3};
    REQUIRE(client.write(finish));
    REQUIRE(receive(active, finish.size()) == finish);
    REQUIRE(active.write(finish));
    REQUIRE(receive(client, finish.size()) == finish);
    REQUIRE(client.write(finish));
    REQUIRE(receive(active, finish.size()) == finish);
    const std::vector<std::uint8_t> clientMarker{9, 0x1c, 0, 0, 0, 0, 0, 0, 0, 99};
    auto spoof = connectMessage("b");
    spoof[1] = 0x16;
    spoof.insert(spoof.end(), clientMarker.begin(), clientMarker.end());
    REQUIRE(client.write(spoof));
    REQUIRE(receive(active, clientMarker.size()) == clientMarker);
    io::EventLoop idle;
    REQUIRE(idle.watch(listeners[1].nativeHandle(), {true, false}, 1));
    auto ready = idle.wait(0);
    REQUIRE(ready);
    CHECK(ready->empty());
    const std::vector<std::uint8_t> backendMarker{9, 0x2d, 0, 0, 0, 0, 0, 0, 0, 99};
    auto unknown = connectMessage("missing");
    const auto same = connectMessage("a");
    unknown.insert(unknown.end(), same.begin(), same.end());
    unknown.insert(unknown.end(), backendMarker.begin(), backendMarker.end());
    REQUIRE(active.write(unknown));
    REQUIRE(receive(client, backendMarker.size()) == backendMarker);
    const auto channel = scenario == Scenario::kLegacy ? "BungeeCord" : "bungeecord:main";
    auto request = connectMessage("b", channel);
    if (scenario == Scenario::kMalformed) {
        request.push_back(0);
        ++request[0];
        REQUIRE(active.write(request));
        CHECK(receive(client, 0, true).empty());
    } else if (scenario == Scenario::kDisabled) {
        request.insert(request.end(), backendMarker.begin(), backendMarker.end());
        REQUIRE(active.write(request));
        CHECK(receive(client, backendMarker.size()) == backendMarker);
    } else {
        std::vector<std::uint8_t> preferences{15, 0x0e, 5, 'e', 'n', '_', 'u', 's', 8, 0, 1, 127, 1, 0, 0, 0};
        REQUIRE(client.write(preferences));
        REQUIRE(receive(active, preferences.size()) == preferences);
        for (const auto target : {1U, 0U}) {
            request = connectMessage(target == 1 ? "b" : "a", channel);
            std::optional<runtime::BackendChangeRequest> control;
            if (scenario == Scenario::kConsoleCommand) {
                const auto reply = runtime::detail::consoleCommand(target == 1 ? "send Tester b" : "send Tester a",
                                                                   proxy.runtime, commandBackends, {});
                REQUIRE(reply.message == "Backend change preparation accepted for Tester.");
            } else if (scenario == Scenario::kConsole) {
                auto submitted = proxy.runtime.sendPlayer("Tester", target == 1 ? "b" : "a");
                REQUIRE(submitted);
                control = std::move(*submitted);
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (!control->result() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(1ms);
                REQUIRE(control->result());
                REQUIRE(*control->result() == runtime::BackendChangeResult::kAccepted);
            } else {
                REQUIRE(active.write(request));
            }
            auto next = acceptBackend(listeners[target]);
            REQUIRE(receive(next, 42).size() == 42);
            control.reset();
            if (scenario == Scenario::kConsole) {
                auto busyRequest = proxy.runtime.sendPlayer("Tester", target == 1 ? "b" : "a");
                REQUIRE(busyRequest);
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (!busyRequest->result() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(1ms);
                REQUIRE(busyRequest->result());
                CHECK(*busyRequest->result() == runtime::BackendChangeResult::kUnavailable);
            }
            if ((scenario == Scenario::kCandidateFailure || scenario == Scenario::kCandidateTimeout) && target == 1) {
                if (scenario == Scenario::kCandidateFailure) {
                    auto wrongIdentity = finished;
                    wrongIdentity[2] ^= 1;
                    REQUIRE(next.write(wrongIdentity));
                }
                CHECK(receive(next, 0, true).empty());
                REQUIRE(active.write(backendMarker));
                REQUIRE(receive(client, backendMarker.size()) == backendMarker);
                REQUIRE(active.write(request));
                next = acceptBackend(listeners[target]);
                REQUIRE(receive(next, 42).size() == 42);
            }
            auto busy = request;
            busy.insert(busy.end(), backendMarker.begin(), backendMarker.end());
            REQUIRE(active.write(busy));
            REQUIRE(receive(client, backendMarker.size()) == backendMarker);
            REQUIRE(next.write(finished));
            REQUIRE(receive(client, 2) == std::vector<std::uint8_t>{1, 0x78});
            REQUIRE(client.write(std::array<std::uint8_t, 2>{1, 0x10}));
            auto settings = preferences;
            settings[1] = 0;
            auto expected = finish;
            expected.insert(expected.end(), settings.begin(), settings.end());
            REQUIRE(receive(next, expected.size()) == expected);
            const std::vector<std::uint8_t> reset{2, 8, 0, 2, 0x10, 0, 2, 0x11, 0, 1, 0x12};
            REQUIRE(receive(client, reset.size()) == reset);
            CHECK(receive(active, 0, true).empty());
            REQUIRE(next.write(std::array<std::uint8_t, 3>{2, 15, 0}));
            REQUIRE(receive(client, 3) == std::vector<std::uint8_t>{2, 15, 0});
            REQUIRE(next.write(finish));
            REQUIRE(receive(client, finish.size()) == finish);
            REQUIRE(client.write(finish));
            REQUIRE(receive(next, finish.size()) == finish);
            REQUIRE(next.write(backendMarker));
            REQUIRE(receive(client, backendMarker.size()) == backendMarker);
            REQUIRE(client.write(clientMarker));
            REQUIRE(receive(next, clientMarker.size()) == clientMarker);
            active = std::move(next);
            CHECK(client.nativeHandle() == publicHandle);
            CHECK(proxy.runtime.sessionCount() == 1);
        }
    }
    proxy.stop();
    CHECK(proxy.runtime.sessionCount() == 0);
    CHECK(proxy.runtime.bufferedBytes() == 0);
    for (auto& listener : listeners) {
        io::EventLoop remaining;
        REQUIRE(remaining.watch(listener.nativeHandle(), {true, false}, 1));
        const auto events = remaining.wait(0);
        REQUIRE(events);
        CHECK(events->empty());
    }
}
