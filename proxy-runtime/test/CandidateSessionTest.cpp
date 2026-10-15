#include "../src/BackendRoutes.hpp"
#include "../src/ClientSession.hpp"
#include "../src/IdentityRegistry.hpp"
#include "RuntimeFixture.hpp"

#include <basalt/wire/auth/forwarding/VelocityForwarding.hpp>

#include <algorithm>

using namespace basalt::proxy;
using namespace basalt::proxy::test;
using namespace basalt::proxy::runtime::detail;

namespace {
const std::vector<std::uint8_t> clearedBackendMetadata{2, 0x08, 0, 2, 0x10, 0, 2, 0x11, 0, 1, 0x12};

struct CandidateFixture {
    config::ProxyConfig config = settings();
    io::ResourceBudget slots{1};
    io::ResourceBudget buffers{4 * 1024 * 1024};
    IdentityRegistry identities{1};
    BackendRoutes routes;
    std::array<io::TcpListener, 2> listeners;
    io::TcpConnection client;
    std::uint16_t port = 0;
    std::optional<BackendAttempt> requested;
    std::stop_source cancellation;
    const std::array<std::uint8_t, 5> unavailable{4, 0, 2, '{', '}'};
    io::EventLoop loop;
    std::array<std::int64_t, 3> watched{-1, -1, -1};
    std::unique_ptr<ClientSession> session;

    explicit CandidateFixture(bool modern = true) {
        config.network.onlineMode = false;
        config.runtime.loginTimeoutMs = 2000;
        config.runtime.connectTimeoutMs = 500;
        config.runtime.readTimeoutMs = 3000;
        const auto address = io::TcpAddress::literal("127.0.0.1", 0);
        REQUIRE(address);
        for (std::size_t i = 0; i < listeners.size(); ++i) {
            auto listener = io::TcpListener::bind(*address);
            REQUIRE(listener);
            const auto backendPort = listener->localPort();
            REQUIRE(backendPort);
            config.backends.push_back({.name = i == 0 ? "a" : "b", .host = "127.0.0.1", .port = *backendPort});
            listeners[i] = std::move(*listener);
        }
        if (modern) {
            config.backends[1].forwarding = basalt::wire::auth::forwarding::ForwardingMode::Velocity;
            config.backends[1].secret = "candidate-secret";
        }
        config.defaultRoute = {"a"};
        auto routing = BackendRoutes::create(config);
        REQUIRE(routing);
        routes = std::move(*routing);
        auto frontend = io::TcpListener::bind(*address);
        REQUIRE(frontend);
        const auto frontendPort = frontend->localPort();
        REQUIRE(frontendPort);
        port = *frontendPort;
        auto connected = io::TcpConnection::connect("127.0.0.1", port);
        REQUIRE(connected);
        REQUIRE(connected->setNonBlocking());
        client = std::move(*connected);
        auto accepted = frontend->accept();
        REQUIRE(accepted);
        REQUIRE(accepted->setNonBlocking());
        auto slot = slots.acquire(1);
        auto storage = buffers.acquire(ClientSession::kBufferBytes);
        REQUIRE(slot);
        REQUIRE(storage);
        session = std::make_unique<ClientSession>(
            std::move(*accepted), std::move(*slot), std::move(*storage), config.runtime,
            std::span<const std::uint8_t>{}, unavailable, ClientSession::Clock::now(), false, false,
            std::span<const std::uint8_t>{}, unavailable, [](auto) { return std::optional<std::stop_source>{}; },
            routes,
            [this](BackendAttempt attempt, auto) {
                requested = attempt;
                cancellation = std::stop_source{};
                return std::optional{cancellation};
            },
            config.backends, buffers, -1, false, identities);
        REQUIRE(session->accounted());
    }

    ~CandidateFixture() {
        session.reset();
        CHECK(slots.used() == 0);
        CHECK(buffers.used() == 0);
    }

    template <class Done>
    void pump(const Done& done) {
        const auto limit = ClientSession::Clock::now() + 2s;
        while (!done() && ClientSession::Clock::now() < limit) {
            REQUIRE(ClientSession::Clock::now() < session->deadline());
            loop.unwatch(session->handle());
            for (const auto handle : watched)
                if (handle >= 0)
                    loop.unwatch(handle);
            if (!session->frontendSuspended())
                REQUIRE(loop.watch(session->handle(), session->interest(), 1));
            const auto endpoints = session->backendWatches();
            for (std::size_t i = 0; i < endpoints.size(); ++i) {
                watched[i] = endpoints[i].handle;
                if (watched[i] >= 0)
                    REQUIRE(loop.watch(watched[i], endpoints[i].interest, i + 2));
            }
            const auto ready = loop.wait(20);
            REQUIRE(ready);
            for (const auto& event : *ready) {
                if (event.token != 1) {
                    const auto current = session->backendWatches();
                    if (std::ranges::none_of(current,
                                             [&](const auto& endpoint) { return endpoint.handle == event.handle; }))
                        continue;
                }
                const bool healthy = event.token == 1 ? session->ready(event) : session->backendReady(event);
                REQUIRE(healthy);
            }
        }
        REQUIRE(done());
    }

    std::vector<std::uint8_t> read(io::TcpConnection& peer, std::size_t size) {
        std::vector<std::uint8_t> result;
        pump([&] {
            std::array<std::uint8_t, 2048> chunk{};
            const auto amount = peer.read(chunk);
            if (amount) {
                REQUIRE(*amount > 0);
                result.insert(result.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*amount));
            } else {
                REQUIRE(amount.error().kind == io::SocketError::Kind::WouldBlock);
            }
            return result.size() >= size;
        });
        return result;
    }

    void forward(io::TcpConnection& peer, std::size_t target = 1) {
        namespace forwarding = basalt::wire::auth::forwarding;
        if (config.backends[target].forwarding != forwarding::ForwardingMode::Velocity)
            return;
        std::vector<std::uint8_t> query{24, 4, 42, 20};
        constexpr std::string_view channel = "velocity:player_info";
        query.insert(query.end(), channel.begin(), channel.end());
        query.push_back(4);
        REQUIRE(peer.write(query));
        const auto response = read(peer, 71);
        REQUIRE(response.size() == 71);
        REQUIRE(response[0] == 70);
        REQUIRE(response[1] == 2);
        REQUIRE(response[2] == 42);
        REQUIRE(response[3] == 1);
        const forwarding::ProxyForwarding policy{.mode = forwarding::ForwardingMode::Velocity,
                                                 .secret = config.backends[target].secret};
        const auto decoded = forwarding::velocity::decode(std::span{response}.subspan(4), policy);
        REQUIRE(forwarding::accepted(decoded));
        CHECK(std::get<forwarding::ForwardedLogin>(decoded).profileId ==
              basalt::wire::common::Uuid::offlinePlayer("Tester"));
    }

    void complete(BackendAttempt attempt, bool success = true) {
        auto storage = buffers.acquire(ClientSession::kResolutionBytes);
        REQUIRE(storage);
        std::vector<io::TcpAddress> addresses;
        if (success) {
            const auto target = io::TcpAddress::literal("127.0.0.1", config.backends[attempt.target].port);
            REQUIRE(target);
            addresses.push_back(*target);
        }
        REQUIRE(session->resolved(attempt, std::move(addresses), std::move(*storage)));
    }

    BackendAttempt resolve() {
        REQUIRE(requested);
        const auto attempt = *requested;
        requested.reset();
        complete(attempt);
        return attempt;
    }

    io::TcpConnection accept(std::size_t index) {
        io::EventLoop accepting;
        REQUIRE(accepting.watch(listeners[index].nativeHandle(), {true, false}, 1));
        const auto ready = accepting.wait(2000);
        REQUIRE(ready);
        REQUIRE_FALSE(ready->empty());
        auto peer = listeners[index].accept();
        REQUIRE(peer);
        REQUIRE(peer->setNonBlocking());
        return std::move(*peer);
    }
};

std::vector<std::uint8_t> success() {
    std::vector<std::uint8_t> result{41,   2,    0xf3, 0xd2, 0x8c, 0xb0, 0x72, 0x25, 0x3c, 0xb1, 0xba, 0xeb, 0x2d,
                                     0xad, 0xd2, 0xbe, 0x89, 0xae, 6,    'T',  'e',  's',  't',  'e',  'r',  0};
    result.insert(result.end(), 16, 0);
    return result;
}

io::TcpConnection startActive(CandidateFixture& fixture) {
    loginStart(fixture.client, fixture.port);
    fixture.pump([&] { return fixture.requested.has_value(); });
    fixture.resolve();
    auto active = fixture.accept(0);
    REQUIRE(fixture.read(active, 42).size() == 42);
    const auto finished = success();
    REQUIRE(active.write(finished));
    REQUIRE(fixture.read(fixture.client, finished.size()) == finished);
    const std::vector<std::uint8_t> ack{1, 3};
    REQUIRE(fixture.client.write(ack));
    REQUIRE(fixture.read(active, ack.size()) == ack);
    REQUIRE(active.write(ack));
    REQUIRE(fixture.read(fixture.client, ack.size()) == ack);
    REQUIRE(fixture.client.write(ack));
    REQUIRE(fixture.read(active, ack.size()) == ack);
    return active;
}

} // namespace

TEST_CASE("a session prepares a second backend while its public connection remains in play") {
    enum class Ending { kAccepted, kIdentity, kEof, kCookieEof, kCookieLimit, kReconfiguration, kExpired };
    auto ending = Ending::kAccepted;
    bool modern = true;
    SUBCASE("candidate reaches validated login without another public login") {}
    SUBCASE("explicit none candidate remains supported") { modern = false; }
    SUBCASE("invalid candidate identity leaves active backend connected") { ending = Ending::kIdentity; }
    SUBCASE("late cookie reply from a closed candidate cannot affect its replacement") { ending = Ending::kCookieEof; }
    SUBCASE("candidate EOF leaves active backend connected") { ending = Ending::kEof; }
    SUBCASE("cookie publication quota rejects only the candidate") { ending = Ending::kCookieLimit; }
    SUBCASE("active reconfiguration cancels candidate preparation") { ending = Ending::kReconfiguration; }
    SUBCASE("candidate preparation expiration leaves active backend connected") { ending = Ending::kExpired; }
    CandidateFixture fixture{modern};
    auto& session = *fixture.session;
    CHECK_FALSE(session.prepareBackend(1));
    auto active = startActive(fixture);
    const auto finished = success();
    const std::vector<std::uint8_t> ack{1, 3};
    const std::vector<std::uint8_t> keepalive{9, 0x2d, 0, 0, 0, 0, 0, 0, 0, 99};
    const auto relayActive = [&] {
        REQUIRE(active.write(keepalive));
        REQUIRE(fixture.read(fixture.client, keepalive.size()) == keepalive);
        auto reply = keepalive;
        reply[1] = 0x1c;
        REQUIRE(fixture.client.write(reply));
        REQUIRE(fixture.read(active, reply.size()) == reply);
    };
    CHECK_FALSE(session.prepareBackend(0));
    REQUIRE(session.prepareBackend(1));
    CHECK_FALSE(session.prepareBackend(1));
    relayActive();
    const auto firstAttempt = fixture.resolve();
    auto candidate = fixture.accept(1);
    REQUIRE(fixture.read(candidate, 42).size() == 42);
    fixture.forward(candidate);
    constexpr std::string_view key = "fixture:test";
    std::vector<std::uint8_t> cookie{14, 5, 12};
    cookie.insert(cookie.end(), key.begin(), key.end());
    const auto cookiePacket = [](std::string_view name, bool reply) {
        std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(name.size() + (reply ? 3 : 2)), 0x15,
                                        static_cast<std::uint8_t>(name.size())};
        bytes.insert(bytes.end(), name.begin(), name.end());
        if (reply)
            bytes.push_back(0);
        return bytes;
    };
    if (ending == Ending::kCookieLimit) {
        std::vector<std::uint8_t> queries;
        for (unsigned i = 0; i < 32; ++i) {
            const auto request = cookiePacket("fixture:pending" + std::to_string(i), false);
            queries.insert(queries.end(), request.begin(), request.end());
        }
        REQUIRE(active.write(queries));
        REQUIRE(fixture.read(fixture.client, queries.size()) == queries);
    }
    REQUIRE(candidate.write(cookie));
    if (ending == Ending::kCookieLimit) {
        fixture.pump([&] { return session.backendWatches()[2].handle < 0; });
        const auto reply = cookiePacket("fixture:pending0", true);
        REQUIRE(fixture.client.write(reply));
        REQUIRE(fixture.read(active, reply.size()) == reply);
        relayActive();
        CHECK(session.accounted());
        return;
    }
    cookie[1] = 0x15;
    REQUIRE(fixture.read(fixture.client, cookie.size()) == cookie);
    std::vector<std::uint8_t> response{15, 0x15, 12};
    response.insert(response.end(), key.begin(), key.end());
    response.push_back(0);
    if (ending == Ending::kCookieEof) {
        candidate.close();
        fixture.pump([&] { return session.backendWatches()[2].handle < 0; });
        auto marker = keepalive;
        marker.back() = 100;
        REQUIRE(active.write(marker));
        REQUIRE(fixture.read(fixture.client, marker.size()) == marker);
        marker[1] = 0x1c;
        auto late = response;
        late.insert(late.end(), marker.begin(), marker.end());
        REQUIRE(fixture.client.write(late));
        REQUIRE(fixture.read(active, marker.size()) == marker);
        relayActive();
        REQUIRE(session.prepareBackend(1));
        REQUIRE(fixture.requested);
        CHECK(fixture.requested->generation != firstAttempt.generation);
        fixture.complete(firstAttempt);
        fixture.resolve();
        auto replacement = fixture.accept(1);
        REQUIRE(fixture.read(replacement, 42).size() == 42);
        fixture.forward(replacement);
        cookie[1] = 5;
        REQUIRE(replacement.write(cookie));
        cookie[1] = 0x15;
        REQUIRE(fixture.read(fixture.client, cookie.size()) == cookie);
        REQUIRE(fixture.client.write(response));
        response[1] = 4;
        REQUIRE(fixture.read(replacement, response.size()) == response);
        REQUIRE(replacement.write(finished));
        fixture.pump([&] { return session.candidatePrepared(); });
        relayActive();
        CHECK(session.accounted());
        return;
    }
    REQUIRE(fixture.client.write(response));
    response[1] = 4;
    REQUIRE(fixture.read(candidate, response.size()) == response);
    if (ending == Ending::kReconfiguration) {
        const std::vector<std::uint8_t> start{1, 0x78};
        REQUIRE(active.write(start));
        REQUIRE(fixture.read(fixture.client, start.size()) == start);
        CHECK(session.backendWatches()[2].handle < 0);
        CHECK_FALSE(session.candidatePrepared());
        const std::vector<std::uint8_t> configurationAck{1, 0x10};
        REQUIRE(fixture.client.write(configurationAck));
        REQUIRE(fixture.read(active, configurationAck.size()) == configurationAck);
        REQUIRE(active.write(std::array<std::uint8_t, 3>{2, 15, 0}));
        REQUIRE(fixture.read(fixture.client, 3) == std::vector<std::uint8_t>{2, 15, 0});
        REQUIRE(active.write(ack));
        REQUIRE(fixture.read(fixture.client, ack.size()) == ack);
        REQUIRE(fixture.client.write(ack));
        REQUIRE(fixture.read(active, ack.size()) == ack);
        relayActive();
        CHECK(session.accounted());
        return;
    }
    auto candidateFinished = finished;
    if (ending == Ending::kIdentity)
        candidateFinished[2] ^= 1;
    REQUIRE(candidate.write(candidateFinished));
    if (ending == Ending::kIdentity) {
        fixture.pump([&] { return session.backendWatches()[2].handle < 0; });
    } else {
        fixture.pump([&] { return session.candidatePrepared(); });
        if (ending == Ending::kEof) {
            candidate.close();
            fixture.pump([&] { return !session.candidatePrepared(); });
        } else if (ending == Ending::kExpired) {
            REQUIRE(session.timedOut(session.deadline()));
            CHECK_FALSE(session.candidatePrepared());
        }
    }
    relayActive();
    if (ending != Ending::kAccepted) {
        REQUIRE(session.prepareBackend(1));
        REQUIRE(fixture.requested);
        CHECK(fixture.requested->generation != firstAttempt.generation);
        fixture.complete(firstAttempt);
        const auto canceled = fixture.cancellation;
        REQUIRE(session.timedOut(session.deadline()));
        CHECK(canceled.stop_requested());
        CHECK_FALSE(session.candidatePrepared());
        relayActive();
    }
    CHECK(session.accounted());
}

TEST_CASE("public configuration acknowledgements commit repeated A B A backend ownership") {
    CandidateFixture fixture;
    auto& session = *fixture.session;
    auto active = startActive(fixture);
    const auto publicHandle = fixture.client.nativeHandle();
    CHECK_FALSE(session.beginPreparedChange());
    const std::vector<std::uint8_t> originalPackId(16, 42);
    const auto responseForPack = [](const std::vector<std::uint8_t>& id, bool configuration, std::uint8_t action) {
        std::vector<std::uint8_t> response{18, static_cast<std::uint8_t>(configuration ? 0x06 : 0x32)};
        response.insert(response.end(), id.begin(), id.end());
        response.push_back(action);
        return response;
    };
    const auto publishPack = [&](io::TcpConnection& peer, bool configuration) {
        std::vector<std::uint8_t> packet{0, static_cast<std::uint8_t>(configuration ? 0x09 : 0x51)};
        packet.insert(packet.end(), originalPackId.begin(), originalPackId.end());
        constexpr std::string_view url = "https://example.invalid/pack.zip";
        packet.push_back(static_cast<std::uint8_t>(url.size()));
        packet.insert(packet.end(), url.begin(), url.end());
        packet.insert(packet.end(), {0, 0, 0});
        packet[0] = static_cast<std::uint8_t>(packet.size() - 1);
        REQUIRE(peer.write(packet));
        const auto received = fixture.read(fixture.client, packet.size());
        REQUIRE(received.size() == packet.size());
        const std::vector<std::uint8_t> presented(received.begin() + 2, received.begin() + 18);
        CHECK(presented != originalPackId);
        std::copy(presented.begin(), presented.end(), packet.begin() + 2);
        CHECK(received == packet);
        return presented;
    };
    auto previousPack = publishPack(active, false);
    REQUIRE(fixture.client.write(responseForPack(previousPack, false, 3)));
    REQUIRE(fixture.read(active, 19) == responseForPack(originalPackId, false, 3));
    std::vector<std::uint8_t> preferences{15, 0x0e, 5, 'e', 'n', '_', 'u', 's', 8, 0, 1, 127, 1, 0, 0, 0};
    REQUIRE(fixture.client.write(preferences));
    REQUIRE(fixture.read(active, preferences.size()) == preferences);
    constexpr std::string_view key = "fixture:late";
    const auto cookie = [&](std::uint8_t id, bool reply, bool present = false) {
        std::vector<std::uint8_t> packet{static_cast<std::uint8_t>(key.size() + 2 + (reply ? (present ? 3 : 1) : 0)),
                                         id, static_cast<std::uint8_t>(key.size())};
        packet.insert(packet.end(), key.begin(), key.end());
        if (reply) {
            packet.push_back(present ? 1 : 0);
            if (present)
                packet.insert(packet.end(), {1, 42});
        }
        return packet;
    };
    const auto oldQuery = cookie(0x15, false);
    REQUIRE(active.write(oldQuery));
    REQUIRE(fixture.read(fixture.client, oldQuery.size()) == oldQuery);
    const std::vector<std::uint8_t> finish{1, 3};
    for (const auto target : {1U, 0U}) {
        REQUIRE(session.prepareBackend(target));
        fixture.resolve();
        auto next = fixture.accept(target);
        REQUIRE(fixture.read(next, 42).size() == 42);
        fixture.forward(next, target);
        CHECK_FALSE(session.beginPreparedChange());
        const auto finished = success();
        REQUIRE(next.write(finished));
        fixture.pump([&] { return session.candidatePrepared(); });
        // An old play packet already in the socket must not become new configuration data.
        const std::vector<std::uint8_t> oldKeepalive{9, 0x2d, 0, 0, 0, 0, 0, 0, 0, 44};
        REQUIRE(active.write(oldKeepalive));
        REQUIRE(session.beginPreparedChange());
        CHECK_FALSE(session.beginPreparedChange());
        REQUIRE(fixture.read(fixture.client, 2) == std::vector<std::uint8_t>{1, 0x78});
        preferences[8] = static_cast<std::uint8_t>(preferences[8] + 4);
        auto acknowledgement = preferences;
        acknowledgement.insert(acknowledgement.end(), {1, 0x10});
        REQUIRE(fixture.client.write(acknowledgement));
        auto configurationSettings = preferences;
        configurationSettings[1] = 0;
        auto expected = finish;
        expected.insert(expected.end(), configurationSettings.begin(), configurationSettings.end());
        REQUIRE(fixture.read(next, expected.size()) == expected);
        REQUIRE(fixture.read(fixture.client, clearedBackendMetadata.size()) == clearedBackendMetadata);
        CHECK_FALSE(session.candidatePrepared());
        const auto currentPack = publishPack(next, true);
        CHECK(currentPack != previousPack);
        auto packResponses = responseForPack(previousPack, true, 7);
        const auto acceptedPack = responseForPack(currentPack, true, 3);
        packResponses.insert(packResponses.end(), acceptedPack.begin(), acceptedPack.end());
        REQUIRE(fixture.client.write(packResponses));
        REQUIRE(fixture.read(next, 19) == responseForPack(originalPackId, true, 3));
        previousPack = currentPack;
        if (target == 0) {
            const auto query = cookie(0, false);
            REQUIRE(next.write(query));
            REQUIRE(fixture.read(fixture.client, query.size()) == query);
            const auto oldResponse = cookie(1, true);
            const auto currentResponse = cookie(1, true, true);
            auto responses = oldResponse;
            responses.insert(responses.end(), currentResponse.begin(), currentResponse.end());
            REQUIRE(fixture.client.write(responses));
            REQUIRE(fixture.read(next, currentResponse.size()) == currentResponse);
        }
        const std::vector<std::uint8_t> newConfiguration{8, 0x10, 1, 1, 'k', 3, 'n', 'e', 'w', 2, 15, 0};
        REQUIRE(next.write(newConfiguration));
        REQUIRE(fixture.read(fixture.client, newConfiguration.size()) == newConfiguration);
        REQUIRE(next.write(finish));
        REQUIRE(fixture.read(fixture.client, finish.size()) == finish);
        REQUIRE(fixture.client.write(finish));
        REQUIRE(fixture.read(next, finish.size()) == finish);
        REQUIRE(fixture.client.write(responseForPack(currentPack, false, 0)));
        REQUIRE(fixture.read(next, 19) == responseForPack(originalPackId, false, 0));
        const std::vector<std::uint8_t> keepalive{9, 0x2d, 0, 0, 0,
                                                  0, 0,    0, 0, static_cast<std::uint8_t>(100 + target)};
        REQUIRE(next.write(keepalive));
        REQUIRE(fixture.read(fixture.client, keepalive.size()) == keepalive);
        auto response = keepalive;
        response[1] = 0x1c;
        REQUIRE(fixture.client.write(response));
        REQUIRE(fixture.read(next, response.size()) == response);
        std::vector<std::uint8_t> removal{18, 0x50, 1};
        removal.insert(removal.end(), originalPackId.begin(), originalPackId.end());
        REQUIRE(next.write(removal));
        std::copy(currentPack.begin(), currentPack.end(), removal.begin() + 3);
        REQUIRE(fixture.read(fixture.client, removal.size()) == removal);
        const auto replacementPack = publishPack(next, false);
        CHECK(replacementPack != currentPack);
        auto afterRemoval = responseForPack(currentPack, false, 7);
        auto replacementAccepted = responseForPack(replacementPack, false, 3);
        afterRemoval.insert(afterRemoval.end(), replacementAccepted.begin(), replacementAccepted.end());
        REQUIRE(fixture.client.write(afterRemoval));
        REQUIRE(fixture.read(next, 19) == responseForPack(originalPackId, false, 3));
        const std::vector<std::uint8_t> removeAll{2, 0x50, 0};
        REQUIRE(next.write(removeAll));
        REQUIRE(fixture.read(fixture.client, removeAll.size()) == removeAll);
        auto lateRemoval = responseForPack(replacementPack, false, 7);
        lateRemoval.insert(lateRemoval.end(), response.begin(), response.end());
        REQUIRE(fixture.client.write(lateRemoval));
        REQUIRE(fixture.read(next, response.size()) == response);
        previousPack = publishPack(next, false);
        CHECK(previousPack != replacementPack);
        active = std::move(next);
        CHECK(fixture.client.nativeHandle() == publicHandle);
        CHECK_FALSE(fixture.identities.claim(basalt::wire::common::Uuid::offlinePlayer("Tester")));
        CHECK(session.accounted());
    }
}

TEST_CASE("public backend change failures close instead of resuming abandoned play") {
    enum class Failure { kBeforeAckTimeout, kAfterAckTimeout, kMalformedAck, kDuplicateAck, kCandidateEof };
    auto failure = Failure::kBeforeAckTimeout;
    SUBCASE("timeout before public ACK") { failure = Failure::kBeforeAckTimeout; }
    SUBCASE("timeout after public ACK") { failure = Failure::kAfterAckTimeout; }
    SUBCASE("ACK with trailing data") { failure = Failure::kMalformedAck; }
    SUBCASE("duplicate public ACK") { failure = Failure::kDuplicateAck; }
    SUBCASE("candidate closes after StartConfiguration") { failure = Failure::kCandidateEof; }
    CandidateFixture fixture;
    auto& session = *fixture.session;
    auto active = startActive(fixture);
    REQUIRE(session.prepareBackend(1));
    fixture.resolve();
    auto next = fixture.accept(1);
    REQUIRE(fixture.read(next, 42).size() == 42);
    fixture.forward(next);
    REQUIRE(next.write(success()));
    fixture.pump([&] { return session.candidatePrepared(); });
    REQUIRE(session.beginPreparedChange());
    REQUIRE(fixture.read(fixture.client, 2) == std::vector<std::uint8_t>{1, 0x78});
    if (failure == Failure::kBeforeAckTimeout) {
        CHECK_FALSE(session.timedOut(session.deadline()));
    } else if (failure == Failure::kCandidateEof) {
        const auto handle = session.backendWatches()[2].handle;
        REQUIRE(handle >= 0);
        next.close();
        io::EventLoop closing;
        REQUIRE(closing.watch(handle, {true, false}, 1));
        const auto events = closing.wait(2000);
        REQUIRE(events);
        REQUIRE_FALSE(events->empty());
        CHECK_FALSE(session.backendReady(events->front()));
    } else {
        const std::vector<std::uint8_t> ack{1, 0x10};
        if (failure != Failure::kMalformedAck) {
            REQUIRE(fixture.client.write(ack));
            REQUIRE(fixture.read(next, 2) == std::vector<std::uint8_t>{1, 3});
            REQUIRE(fixture.read(fixture.client, clearedBackendMetadata.size()) == clearedBackendMetadata);
        }
        if (failure == Failure::kAfterAckTimeout) {
            CHECK_FALSE(session.timedOut(session.deadline()));
        } else {
            const auto packet = failure == Failure::kMalformedAck ? std::vector<std::uint8_t>{2, 0x10, 0} : ack;
            REQUIRE(fixture.client.write(packet));
            io::EventLoop input;
            REQUIRE(input.watch(session.handle(), {true, false}, 1));
            const auto events = input.wait(2000);
            REQUIRE(events);
            REQUIRE_FALSE(events->empty());
            CHECK_FALSE(session.ready(events->front()));
        }
    }
    fixture.session.reset();
    CHECK(receive(fixture.client, 0, true).empty());
}

TEST_CASE("resource pack control validates IDs actions and complete response payloads") {
    enum class Input {
        kShortResponse,
        kInvalidAction,
        kTrailingResponse,
        kUnknownResponse,
        kInvalidPop,
        kTrailingPop,
        kShortPush
    };
    auto input = Input::kShortResponse;
    SUBCASE("short response UUID") {}
    SUBCASE("action outside native enum") { input = Input::kInvalidAction; }
    SUBCASE("trailing response bytes") { input = Input::kTrailingResponse; }
    SUBCASE("unknown response cannot reach backend") { input = Input::kUnknownResponse; }
    SUBCASE("invalid pop presence") { input = Input::kInvalidPop; }
    SUBCASE("trailing pop bytes") { input = Input::kTrailingPop; }
    SUBCASE("truncated push UUID") { input = Input::kShortPush; }
    CandidateFixture fixture;
    auto& session = *fixture.session;
    auto active = startActive(fixture);
    const bool fromBackend = input == Input::kInvalidPop || input == Input::kTrailingPop || input == Input::kShortPush;
    std::vector<std::uint8_t> packet{18, 0x32};
    packet.insert(packet.end(), 16, 42);
    packet.push_back(input == Input::kInvalidAction ? 8 : 3);
    if (input == Input::kShortResponse)
        packet.resize(10);
    if (input == Input::kTrailingResponse)
        packet.push_back(0);
    if (input == Input::kInvalidPop)
        packet = {2, 0x50, 2};
    if (input == Input::kTrailingPop)
        packet = {3, 0x50, 0, 0};
    if (input == Input::kShortPush)
        packet = {2, 0x51, 0};
    packet[0] = static_cast<std::uint8_t>(packet.size() - 1);
    if (input == Input::kUnknownResponse) {
        const std::vector<std::uint8_t> marker{9, 0x1c, 0, 0, 0, 0, 0, 0, 0, 99};
        packet.insert(packet.end(), marker.begin(), marker.end());
        REQUIRE(fixture.client.write(packet));
        CHECK(fixture.read(active, marker.size()) == marker);
        return;
    }
    REQUIRE((fromBackend ? active : fixture.client).write(packet));
    const auto handle = fromBackend ? session.backendWatches()[1].handle : session.handle();
    REQUIRE(handle >= 0);
    io::EventLoop readable;
    REQUIRE(readable.watch(handle, {true, false}, 1));
    const auto events = readable.wait(2000);
    REQUIRE(events);
    REQUIRE_FALSE(events->empty());
    const bool healthy = fromBackend ? session.backendReady(events->front()) : session.ready(events->front());
    CHECK_FALSE(healthy);
}

TEST_CASE("requested change waits for frontend backpressure before starting public configuration") {
    CandidateFixture fixture{false};
    auto& session = *fixture.session;
    auto active = startActive(fixture);
    REQUIRE(session.requestBackendChange(1));
    fixture.resolve();
    auto next = fixture.accept(1);
    REQUIRE(fixture.read(next, 42).size() == 42);
    fixture.config.runtime.highWaterBytes = 1;
    fixture.config.runtime.lowWaterBytes = 0;
    const std::vector<std::uint8_t> marker{9, 0x1c, 0, 0, 0, 0, 0, 0, 0, 99};
    REQUIRE(fixture.client.write(marker));
    io::EventLoop input;
    REQUIRE(input.watch(session.handle(), {true, false}, 1));
    const auto front = input.wait(2000);
    REQUIRE(front);
    REQUIRE_FALSE(front->empty());
    REQUIRE(session.ready(front->front()));
    REQUIRE(session.frontendSuspended());
    REQUIRE(next.write(success()));
    const auto candidateHandle = session.backendWatches()[2].handle;
    io::EventLoop preparing;
    REQUIRE(preparing.watch(candidateHandle, {true, false}, 1));
    const auto candidate = preparing.wait(2000);
    REQUIRE(candidate);
    REQUIRE_FALSE(candidate->empty());
    REQUIRE(session.backendReady(candidate->front()));
    REQUIRE(session.candidatePrepared());
    CHECK_FALSE(session.beginPreparedChange());
    const auto activeHandle = session.backendWatches()[1].handle;
    REQUIRE(session.backendReady({.handle = activeHandle, .writable = true}));
    REQUIRE(fixture.read(active, marker.size()) == marker);
    REQUIRE(fixture.read(fixture.client, 2) == std::vector<std::uint8_t>{1, 0x78});
    REQUIRE(fixture.client.write(std::array<std::uint8_t, 2>{1, 0x10}));
    REQUIRE(fixture.read(next, 2) == std::vector<std::uint8_t>{1, 3});
    REQUIRE(fixture.read(fixture.client, clearedBackendMetadata.size()) == clearedBackendMetadata);
    CHECK_FALSE(session.candidatePrepared());
    CHECK(session.accounted());
}

TEST_CASE("client Bungee Connect cannot publish a backend resolution request") {
    CandidateFixture fixture{false};
    fixture.config.backends[0].bungeeMessages = true;
    auto active = startActive(fixture);
    std::vector<std::uint8_t> request{26,  0x16, 15,  'b', 'u', 'n', 'g', 'e', 'e', 'c', 'o', 'r', 'd', ':', 'm',
                                      'a', 'i',  'n', 0,   7,   'C', 'o', 'n', 'n', 'e', 'c', 't', 0,   1,   'b'};
    request[0] = static_cast<std::uint8_t>(request.size() - 1);
    const std::vector<std::uint8_t> marker{9, 0x1c, 0, 0, 0, 0, 0, 0, 0, 99};
    auto spoof = request;
    spoof.insert(spoof.end(), marker.begin(), marker.end());
    REQUIRE(fixture.client.write(spoof));
    REQUIRE(fixture.read(active, marker.size()) == marker);
    CHECK_FALSE(fixture.requested);
    CHECK(fixture.session->backendWatches()[2].handle < 0);
    request[1] = 0x18;
    REQUIRE(active.write(request));
    fixture.pump([&] { return fixture.requested.has_value(); });
    REQUIRE(fixture.requested);
    CHECK(fixture.requested->target == 1);
}
