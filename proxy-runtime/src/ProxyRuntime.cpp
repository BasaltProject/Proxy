#include "AccessPolicy.hpp"
#include "BackendChangeState.hpp"
#include "BackendRoutes.hpp"
#include "ClientSession.hpp"
#include "ForwardingKey.hpp"
#include "NativeForwarding.hpp"
#include "RelayCounters.hpp"
#include "ServerHealth.hpp"
#include "StatusRelay.hpp"
#include "antibot/AttackReport.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/Admission.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/auth/SessionKeyPair.hpp>
#include <basalt/wire/auth/forwarding/ProxyForwarding.hpp>
#include <basalt/wire/io/EventLoop.hpp>
#include <basalt/wire/io/ResourceBudget.hpp>
#include <basalt/wire/io/TaskQueue.hpp>
#include <basalt/wire/io/TimerQueue.hpp>
#include <basalt/wire/io/WorkerPool.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>
#include <basalt/wire/text/Component.hpp>
#include <basalt/wire/text/MiniMessage.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace basalt::proxy::runtime {
namespace {
struct Topology {
    std::vector<config::BackendConfig> backends;
    std::vector<config::ServerGroup> groups;
    detail::BackendRoutes routes;
    std::shared_ptr<const detail::NativeForwarding> forwarding;
};

/// libc++ has no std::atomic<std::shared_ptr>, so a mutex guards the swap and readers copy the pointer.
class SnapshotCell {
public:
    [[nodiscard]] std::shared_ptr<const Topology> load() const {
        std::lock_guard lock{mutex_};
        return value_;
    }

    void store(std::shared_ptr<const Topology> value) {
        std::lock_guard lock{mutex_};
        value_ = std::move(value);
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const Topology> value_;
};

std::int32_t nativeProtocol(const config::BackendConfig& server) {
    return server.protocol == config::BackendConfig::kClientProtocol ? buildInfo::kProtocolVersion : server.protocol;
}

std::expected<std::shared_ptr<const detail::NativeForwarding>, std::string>
loadForwarding(const config::ProxyConfig& config) {
    if (std::ranges::none_of(config.backends, [](const config::BackendConfig& backend) {
            return backend.forwarding == wire::auth::forwarding::ForwardingMode::Basalt;
        }))
        return nullptr;
    auto signing = detail::loadOrCreateForwardingKey(config.forwarding.keyFile);
    if (!signing)
        return std::unexpected(signing.error());
    return std::make_shared<const detail::NativeForwarding>(
        detail::NativeForwarding{std::move(*signing), config.forwarding.proxyId, config.network.onlineMode,
                                 config.translation == config::TranslationMode::Auto});
}
} // namespace

namespace {
using Session = detail::ClientSession;
using Clock = Session::Clock;
using wire::io::ResourceBudget;

struct Incoming {
    ResourceBudget::Reservation slot;
    ResourceBudget::Reservation buffers;
    ResourceBudget::Reservation handoff;
    wire::io::TcpConnection connection;
    Clock::time_point acceptedAt;
    detail::ClientAccess access;
};

struct AuthenticationReply {
    ResourceBudget::Reservation storage;
    std::expected<wire::auth::AuthenticatedLogin, wire::auth::LoginAuthenticationFailure> result;
};

struct ResolutionReply {
    ResourceBudget::Reservation storage;
    std::vector<wire::io::TcpAddress> addresses;
};
} // namespace

struct ProxyRuntime::Impl {
    struct Owner {
        Impl& runtime;
        wire::io::EventLoop loop;
        wire::io::TaskQueue mailbox;
        wire::io::TimerQueue timers;
        std::unordered_map<std::uint64_t, std::unique_ptr<Session>> sessions;
        std::vector<wire::io::IoReady> ready;

        explicit Owner(Impl& parent)
            : runtime(parent), mailbox(parent.config.runtime.maxPendingWork),
              timers(parent.config.runtime.maxSessions) {
            sessions.reserve(parent.config.runtime.maxSessions);
            ready.reserve(parent.config.runtime.maxSessions + 1);
        }

        void unwatchBackends(Session& session) {
            for (auto& handle : session.watchedBackends) {
                if (handle >= 0)
                    loop.unwatch(handle);
                handle = -1;
            }
        }

        void close(std::uint64_t id) {
            const auto found = sessions.find(id);
            if (found == sessions.end())
                return;
            if (found->second->announced) {
                runtime.forget(found->second->playerName());
                if (runtime.services.playerEvents)
                    runtime.services.playerEvents({PlayerEventKind::kLeave,
                                                   std::string{found->second->playerName()},
                                                   found->second->announcedBackend,
                                                   found->second->playerUuid(),
                                                   {}});
            }
            detail::relayCounters().endedIn[found->second->phase()].fetch_add(1, std::memory_order_relaxed);
            loop.unwatch(found->second->handle());
            unwatchBackends(*found->second);
            (void)timers.cancel(found->second->timer);
            sessions.erase(found);
        }

        void add(std::shared_ptr<Incoming> incoming) {
            incoming->handoff.reset();
            if (runtime.stopped.load() ||
                Clock::now() >=
                    incoming->acceptedAt + std::chrono::milliseconds{runtime.config.runtime.handshakeTimeoutMs})
                return;
            const auto id = runtime.nextId.fetch_add(1);
            if (id == 0)
                throw std::runtime_error("session identity exhausted");
            const auto snapshot = runtime.topology.load();
            auto session = std::make_unique<Session>(
                std::move(incoming->connection), std::move(incoming->slot), std::move(incoming->buffers),
                runtime.config.runtime, runtime.statusSource, runtime.unavailable, incoming->acceptedAt,
                runtime.config.network.onlineMode, runtime.config.network.preventClientProxyConnections,
                runtime.key ? runtime.key->publicKeyDer() : std::span<const std::uint8_t>{},
                runtime.authenticationFailure,
                [this, id](wire::auth::OnlineLoginRequest request) { return authenticate(id, std::move(request)); },
                snapshot->routes,
                [this, id, snapshot](detail::BackendAttempt attempt, Clock::time_point deadline) {
                    return resolve(id, attempt, deadline, snapshot);
                },
                snapshot->backends, runtime.buffers, runtime.config.network.compressionThreshold,
                runtime.config.network.acceptTransfers, runtime.identities, snapshot->forwarding.get(), snapshot,
                runtime.services.playerCommand, runtime.services.playerCommandSpecs, runtime.config.translation,
                runtime.services.playerChat, runtime.services.playerKick, runtime.services.statusPing,
                runtime.services.playerLogin, runtime.services.kickText, runtime.services.commandSuggestions,
                runtime.services.loadingMessage, std::move(incoming->access), runtime.services.pluginMessage,
                runtime.services.decide, runtime.services.latency);
            if (runtime.services.uiConfigs && runtime.services.placeholders != nullptr)
                session->useUi({runtime.services.uiConfigs, runtime.services.placeholders});
            if (!session->accounted())
                return;
            if (runtime.services.packets)
                session->attachPackets(runtime.services.packets, id, runtime.services.packetMaxBytes);
            auto timer = timers.schedule(session->deadline(), id);
            if (!timer)
                return;
            session->timer = *timer;
            auto watched = loop.watch(session->handle(), session->interest(), id);
            if (!watched) {
                (void)timers.cancel(*timer);
                return;
            }
            sessions.emplace(id, std::move(session));
        }

        void publishAuthentication(std::uint64_t id, std::shared_ptr<AuthenticationReply> reply) {
            auto permit = runtime.handoffs.acquire(1);
            if (!permit)
                return;
            auto ticket = std::make_shared<ResourceBudget::Reservation>(std::move(*permit));
            auto posted = mailbox.push([this, id, reply, ticket] {
                ticket->reset();
                const auto found = sessions.find(id);
                if (found == sessions.end())
                    return;
                auto& session = *found->second;
                if (!session.authenticated(std::move(reply->result), std::move(reply->storage)) ||
                    !refresh(id, session))
                    close(id);
            });
            if (posted)
                (void)loop.wake();
        }

        std::optional<std::stop_source> authenticate(std::uint64_t id, wire::auth::OnlineLoginRequest request) {
            if (!runtime.key || !runtime.workers)
                return std::nullopt;
            auto storage = runtime.buffers.acquire(Session::kAuthenticationBytes);
            if (!storage)
                return std::nullopt;
            using Error = wire::auth::LoginAuthenticationError;
            auto reply = std::make_shared<AuthenticationReply>(AuthenticationReply{
                std::move(*storage), std::unexpected(wire::auth::LoginAuthenticationFailure{Error::Unavailable, {}})});
            auto submitted = runtime.workers->submit(
                [this, id, request = std::move(request), reply](std::stop_token stop) {
                    reply->result =
                        wire::auth::authenticateLogin(*runtime.key, request, stop, runtime.services.authentication);
                    publishAuthentication(id, reply);
                },
                [this, id, reply](std::exception_ptr) { publishAuthentication(id, reply); },
                [this, id, reply] {
                    reply->result = std::unexpected(wire::auth::LoginAuthenticationFailure{Error::Canceled, {}});
                    publishAuthentication(id, reply);
                });
            if (!submitted)
                return std::nullopt;
            return *submitted;
        }

        void announce(Session& session) {
            if (!session.inPlay())
                return;
            const auto backend = session.activeBackendName();
            if (!session.announced) {
                session.announced = true;
                session.announcedBackend = backend;
                runtime.remember(session.playerName());
                if (runtime.services.playerEvents)
                    runtime.services.playerEvents({PlayerEventKind::kJoin,
                                                   std::string{session.playerName()},
                                                   session.announcedBackend,
                                                   session.playerUuid(),
                                                   {},
                                                   session.clientAddress(),
                                                   session.clientPort()});
            } else if (backend != session.announcedBackend) {
                const std::string from = session.announcedBackend;
                session.announcedBackend = backend;
                if (runtime.services.playerEvents)
                    runtime.services.playerEvents({PlayerEventKind::kSwitch, std::string{session.playerName()},
                                                   session.announcedBackend, session.playerUuid(), from});
            }
        }

        bool refresh(std::uint64_t id, Session& session) {
            announce(session);
            if (session.frontendSuspended())
                loop.unwatch(session.handle());
            else if (!loop.watch(session.handle(), session.interest(), id))
                return false;
            const auto endpoints = session.backendWatches();
            for (auto old : session.watchedBackends) {
                if (old >= 0 && std::ranges::none_of(endpoints, [old](const auto& next) { return next.handle == old; }))
                    loop.unwatch(old);
            }
            session.watchedBackends.fill(-1);
            for (std::size_t i = 0; i < endpoints.size(); ++i) {
                const auto& endpoint = endpoints[i];
                if (endpoint.handle < 0)
                    continue;
                if (!loop.watch(endpoint.handle, endpoint.interest, id)) {
                    for (const auto& pending : endpoints)
                        if (pending.handle >= 0)
                            loop.unwatch(pending.handle);
                    session.watchedBackends.fill(-1);
                    return false;
                }
                session.watchedBackends[i] = endpoint.handle;
            }
            return timers.reschedule(session.timer, session.deadline());
        }

        void publishResolution(std::uint64_t id, detail::BackendAttempt attempt,
                               std::shared_ptr<ResolutionReply> reply) {
            auto permit = runtime.handoffs.acquire(1);
            if (!permit)
                return;
            auto ticket = std::make_shared<ResourceBudget::Reservation>(std::move(*permit));
            if (mailbox.push([this, id, attempt, reply, ticket] {
                    ticket->reset();
                    const auto found = sessions.find(id);
                    if (found == sessions.end())
                        return;
                    auto& session = *found->second;
                    if (!session.resolved(attempt, std::move(reply->addresses), std::move(reply->storage)) ||
                        !refresh(id, session))
                        close(id);
                }))
                (void)loop.wake();
        }

        std::optional<std::stop_source> resolve(std::uint64_t id, detail::BackendAttempt attempt,
                                                Clock::time_point deadline,
                                                const std::shared_ptr<const Topology>& snapshot) {
            if (!runtime.workers || attempt.target >= snapshot->backends.size())
                return std::nullopt;
            auto storage = runtime.buffers.acquire(Session::kResolutionBytes);
            if (!storage)
                return std::nullopt;
            auto reply = std::make_shared<ResolutionReply>(ResolutionReply{std::move(*storage), {}});
            auto submitted = runtime.workers->submit(
                [this, id, attempt, deadline, reply, snapshot](std::stop_token stop) {
                    const auto& backend = snapshot->backends[attempt.target];
                    const wire::io::DnsOptions options{deadline, stop, {}};
                    auto addresses = runtime.services.resolution
                                         ? runtime.services.resolution(backend.host, backend.port, options)
                                         : wire::io::resolveDns(backend.host, backend.port, options);
                    if (addresses && addresses->size() <= 32 &&
                        addresses->capacity() <= reply->storage.size() / sizeof(wire::io::TcpAddress))
                        reply->addresses = std::move(*addresses);
                    publishResolution(id, attempt, reply);
                },
                [this, id, attempt, reply](std::exception_ptr) { publishResolution(id, attempt, reply); },
                [this, id, attempt, reply] { publishResolution(id, attempt, reply); });
            return submitted ? std::optional{*submitted} : std::nullopt;
        }

        void accept() {
            // Bound accepts per readiness batch so existing sessions and stop cannot starve.
            for (int attempts = 0; attempts < 32 && !runtime.stopped.load(); ++attempts) {
                auto connection = runtime.listener.accept();
                if (!connection) {
                    if (connection.error().kind == wire::io::SocketError::Kind::WouldBlock)
                        return;
                    throw std::runtime_error("listener accept failed");
                }
                const auto now = Clock::now();
                const auto settings = runtime.access.settings();
                detail::ClientAccess origin{&runtime.access,
                                            settings,
                                            {},
                                            settings->trustsUpstream(connection->remoteAddress()),
                                            runtime.services.isOperator};
                if (!origin.awaitHeader) {
                    if (runtime.admission.attempt(connection->remoteAddress(), now) != AdmissionResult::Accepted) {
                        runtime.rejected.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    if (runtime.access.antibot.connection(antibot::originOf(connection->remoteAddress()), now).action ==
                        antibot::Action::kDeny) {
                        runtime.rejected.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    origin.slot = runtime.access.gate.open(connection->remoteAddress(),
                                                           settings->network.maxConnectionsPerAddress);
                    if (!origin.slot) {
                        runtime.rejected.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                }
                auto slot = runtime.slots.acquire(1);
                auto allocation = runtime.buffers.acquire(Session::kBufferBytes);
                auto handoff = runtime.handoffs.acquire(1);
                if (!slot || !allocation || !handoff || !connection->setNonBlocking()) {
                    runtime.rejected.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                runtime.accepted.fetch_add(1, std::memory_order_relaxed);
                auto incoming =
                    std::make_shared<Incoming>(Incoming{std::move(*slot), std::move(*allocation), std::move(*handoff),
                                                        std::move(*connection), now, std::move(origin)});
                auto& target = *runtime.owners[runtime.nextOwner++ % runtime.owners.size()];
                auto submitted = target.mailbox.push([&target, incoming] { target.add(incoming); });
                if (submitted)
                    (void)target.loop.wake();
            }
        }

        void run(bool listenerOwner) {
            try {
                if (listenerOwner && !loop.watch(runtime.listener.nativeHandle(), {true, false}, 0))
                    throw std::runtime_error("listener registration failed");
                while (!runtime.stopped.load()) {
                    while (auto task = mailbox.pop()) {
                        if (runtime.stopped.load())
                            break;
                        (*task)();
                    }
                    const auto now = Clock::now();
                    for (const auto& timer : timers.expire(now)) {
                        const auto found = sessions.find(timer.token);
                        if (found == sessions.end())
                            continue;
                        auto& session = *found->second;
                        unwatchBackends(session);
                        if (!session.timedOut(now)) {
                            close(timer.token);
                            continue;
                        }
                        auto replacement = timers.schedule(session.deadline(), timer.token);
                        if (!replacement) {
                            close(timer.token);
                            continue;
                        }
                        session.timer = *replacement;
                        if (!refresh(timer.token, session))
                            close(timer.token);
                    }
                    auto events = loop.wait(timers.nextTimeout(now));
                    if (!events)
                        throw std::runtime_error("readiness wait failed");
                    ready.assign(events->begin(), events->end());
                    for (const auto& event : ready) {
                        if (runtime.stopped.load())
                            break;
                        if (event.token == 0) {
                            accept();
                            continue;
                        }
                        const auto found = sessions.find(event.token);
                        if (found == sessions.end())
                            continue;
                        auto& session = *found->second;
                        bool healthy = false;
                        if (Clock::now() >= session.deadline()) {
                            unwatchBackends(session);
                            healthy = session.timedOut(Clock::now());
                        } else if (event.handle == session.handle()) {
                            healthy = session.ready(event);
                        } else if (std::ranges::find(session.watchedBackends, event.handle) !=
                                   session.watchedBackends.end()) {
                            const auto before = session.backendSerials();
                            const bool dialing = session.dialing();
                            healthy = session.backendReady(event);
                            if (healthy && (dialing || session.backendSerials() != before))
                                loop.unwatch(event.handle);
                        } else {
                            continue;
                        }
                        if (!healthy || !refresh(event.token, session))
                            close(event.token);
                    }
                }
            } catch (...) {
                runtime.failed.store(true);
                runtime.stop();
            }
            mailbox.close();
            while (!sessions.empty())
                close(sessions.begin()->first);
            if (listenerOwner) {
                loop.unwatch(runtime.listener.nativeHandle());
                runtime.listener.close();
            }
        }
    };

    config::ProxyConfig config;
    SnapshotCell topology;
    Admission admission;
    detail::AccessPolicy access;
    std::unique_ptr<antibot::AttackReport> report;
    ResourceBudget slots;
    ResourceBudget buffers;
    ResourceBudget handoffs;
    ResourceBudget controls;
    std::optional<ResourceBudget::Reservation> bootstrapBuffers;
    wire::io::TcpListener listener;
    std::uint16_t port = 0;
    detail::ClientSession::StatusSource statusSource;
    std::vector<std::uint8_t> unavailable;
    std::vector<std::uint8_t> authenticationFailure;
    RuntimeServices services;
    std::optional<wire::auth::SessionKeyPair> key;
    std::optional<wire::io::WorkerPool> workers;
    detail::IdentityRegistry identities;
    std::vector<std::unique_ptr<Owner>> owners;
    std::vector<std::jthread> threads;
    // Apart from `threads`, whose clear() is how run() waits: it would stop this one the moment it started.
    std::jthread health;
    std::jthread serviceKeys;
    std::unique_ptr<detail::StatusRelay> relay;
    std::atomic<bool> stopped{false};
    std::shared_ptr<std::atomic<bool>> controlStopped = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> failed{false};
    std::atomic<bool> started{false};
    std::atomic<std::uint64_t> nextId{1};
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::size_t nextOwner = 0;
    mutable std::mutex namesMutex;
    std::vector<std::string> names;

    void remember(std::string_view player) {
        std::lock_guard lock{namesMutex};
        names.emplace_back(player);
    }

    void forget(std::string_view player) {
        std::lock_guard lock{namesMutex};
        if (const auto found = std::ranges::find(names, player); found != names.end())
            names.erase(found);
    }

    explicit Impl(config::ProxyConfig settings)
        : config(std::move(settings)), admission(config.admission), access(config, admission),
          slots(config.runtime.maxSessions), buffers(config.runtime.maxBufferedBytes),
          handoffs(config.runtime.maxPendingWork), controls(config.runtime.maxPendingWork),
          identities(config.runtime.maxSessions) {}

    void stop() {
        stopped.store(true);
        controlStopped->store(true, std::memory_order_release);
        if (workers)
            workers->requestStop();
        for (const auto& owner : owners)
            (void)owner->loop.wake();
    }

    ~Impl() {
        stop();
        threads.clear();
        health = {};
        if (workers)
            workers->join();
    }
};

ProxyRuntime::ProxyRuntime(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ProxyRuntime::ProxyRuntime(ProxyRuntime&&) noexcept = default;
ProxyRuntime& ProxyRuntime::operator=(ProxyRuntime&&) noexcept = default;

ProxyRuntime::~ProxyRuntime() = default;

std::expected<ProxyRuntime, std::string> ProxyRuntime::create(config::ProxyConfig config, RuntimeServices services) {
    const auto& limits = config.runtime;
    if (limits.ioLoops == 0 || limits.ioLoops > 64 || limits.maxSessions == 0 || limits.maxSessions > 65536 ||
        limits.maxPendingWork == 0 || limits.maxPendingWork > 65536 ||
        limits.sessionBufferBytes < Session::kBufferBytes || limits.maxBufferedBytes < Session::kBufferBytes ||
        limits.handshakeTimeoutMs == 0 || limits.loginTimeoutMs == 0 || limits.connectTimeoutMs == 0 ||
        limits.readTimeoutMs == 0 || limits.writeTimeoutMs == 0 || limits.lowWaterBytes >= limits.highWaterBytes ||
        limits.highWaterBytes >= limits.sessionBufferBytes)
        return std::unexpected("invalid runtime resource limits");
    auto address = wire::io::TcpAddress::literal(config.network.bind, config.network.port);
    if (!address)
        return std::unexpected("invalid listener address");
    if (config.network.dualStack && config.network.bind.find(':') == std::string::npos)
        return std::unexpected("dual-stack requires an IPv6 listener");
    try {
        auto impl = std::make_unique<Impl>(std::move(config));
        auto routes = detail::BackendRoutes::create(impl->config);
        if (!routes)
            return std::unexpected(routes.error());
        auto forwarding = loadForwarding(impl->config);
        if (!forwarding)
            return std::unexpected(forwarding.error());
        impl->topology.store(std::make_shared<const Topology>(
            Topology{impl->config.backends, impl->config.serverGroups, std::move(*routes), std::move(*forwarding)}));
        impl->services = std::move(services);
        impl->report = std::make_unique<antibot::AttackReport>(impl->access.antibot);
        {
            impl->access.antibot.observe(
                [report = impl->report.get(), events = impl->services.playerEvents](const antibot::Event& event) {
                    report->onEvent(event);
                    if (!events)
                        return;
                    using Kind = antibot::Event::Kind;
                    const std::string_view kind = event.kind == Kind::kVerdict         ? "verdict"
                                                  : event.kind == Kind::kAttackStarted ? "attack_started"
                                                  : event.kind == Kind::kAttackEnded   ? "attack_ended"
                                                  : event.kind == Kind::kVerified      ? "verified"
                                                  : event.kind == Kind::kBanned        ? "banned"
                                                                                       : "limbo_failed";
                    events({PlayerEventKind::kAntibot, event.player, std::string{kind},
                            std::string{antibot::actionName(event.verdict.action)} + ":" +
                                std::string{antibot::reasonName(event.verdict.reason)} + ":" +
                                std::to_string(event.skipped),
                            event.origin});
                });
        }
        impl->relay = std::make_unique<detail::StatusRelay>(
            impl->config.statusRelay,
            [state = impl.get()](const std::string& name) -> std::optional<detail::ProbeTarget> {
                const auto topology = state->topology.load();
                const auto found = std::ranges::find(topology->backends, name, &config::BackendConfig::name);
                if (found == topology->backends.end())
                    return std::nullopt;
                return detail::ProbeTarget{found->host, found->port, nativeProtocol(*found)};
            });
        if (impl->config.network.onlineMode) {
            impl->key = wire::auth::SessionKeyPair::generate();
            if (!impl->key)
                return std::unexpected("could not generate the login key");
        }
        auto workers = wire::io::WorkerPool::create(impl->config.runtime.workers, impl->config.runtime.maxPendingWork);
        if (!workers)
            return std::unexpected("could not start bounded runtime workers");
        impl->workers = std::move(*workers);
        namespace protocol = wire::protocol;
        wire::io::ByteBuffer packet{1024}, frame{1024};
        impl->statusSource = [state = impl.get()](std::int32_t clientProtocol, std::string_view host,
                                                  std::string_view address,
                                                  const DecisionResult* changes) -> std::vector<std::uint8_t> {
            protocol::status::ServerStatusPayload payload =
                state->services.status
                    ? state->services.status(clientProtocol, host, address)
                    : protocol::status::ServerStatusPayload{
                          .description = wire::text::Component::text("Basalt Proxy"),
                          .players =
                              protocol::status::ServerStatusPayload::Players{
                                  .max = static_cast<std::int32_t>(state->config.runtime.maxSessions),
                                  .online = 0,
                                  .sample = {}},
                          .version = protocol::status::ServerStatusPayload::Version{std::string{buildInfo::kVersion},
                                                                                    buildInfo::kProtocolVersion},
                          .favicon = std::nullopt};
            if (changes != nullptr) {
                if (!changes->text.empty())
                    payload.description = wire::text::miniMessage::parse(changes->text);
                if (changes->maxPlayers && payload.players)
                    payload.players->max = *changes->maxPlayers;
                if (changes->online && payload.players)
                    payload.players->online = *changes->online;
            }
            payload.enforcesSecureChat = state->config.general.enforceSecureProfile && state->config.network.onlineMode;
            wire::io::ByteBuffer body{1024}, framed{1024};
            protocol::varInt::write(body, protocol::status::StatusResponsePacket::kId);
            protocol::status::StatusResponsePacket{std::move(payload)}.write(body);
            if (!protocol::packetFraming::writeUncompressed(framed, body.readable()))
                return {};
            return framed.toVector();
        };
        packet.clear();
        frame.clear();
        protocol::varInt::write(packet, protocol::login::LoginDisconnectPacket::kId);
        protocol::login::LoginDisconnectPacket{
            wire::text::Component::text("No server is available right now. Please try again shortly.")}
            .write(packet);
        if (!protocol::packetFraming::writeUncompressed(frame, packet.readable()))
            return std::unexpected("login rejection encoding failed");
        impl->unavailable = frame.toVector();
        packet.clear();
        frame.clear();
        protocol::varInt::write(packet, protocol::login::LoginDisconnectPacket::kId);
        protocol::login::LoginDisconnectPacket{wire::text::Component::text("Login could not be completed.")}.write(
            packet);
        if (!protocol::packetFraming::writeUncompressed(frame, packet.readable()))
            return std::unexpected("authentication rejection encoding failed");
        impl->authenticationFailure = frame.toVector();
        if (impl->unavailable.size() > Session::kBufferCapacity)
            return std::unexpected("bootstrap responses exceed buffer limits");
        impl->bootstrapBuffers =
            impl->buffers.acquire(impl->unavailable.capacity() + impl->authenticationFailure.capacity());
        if (!impl->bootstrapBuffers || impl->buffers.limit() - impl->buffers.used() < Session::kBufferBytes)
            return std::unexpected("global buffer budget cannot admit a bootstrap session");
        impl->owners.reserve(impl->config.runtime.ioLoops);
        impl->threads.reserve(impl->config.runtime.ioLoops);
        for (std::size_t i = 0; i < impl->config.runtime.ioLoops; ++i)
            impl->owners.push_back(std::make_unique<Impl::Owner>(*impl));
        auto bound = wire::io::TcpListener::bind(*address, 128, impl->config.network.dualStack);
        if (!bound || !bound->setNonBlocking())
            return std::unexpected("could not bind the public listener on " + impl->config.network.bind + ":" +
                                   std::to_string(impl->config.network.port) + ", is the port already in use?");
        auto port = bound->localPort();
        if (!port)
            return std::unexpected("could not query the listener port");
        impl->port = *port;
        impl->listener = std::move(*bound);
        return ProxyRuntime{std::move(impl)};
    } catch (const std::exception& error) {
        return std::unexpected(std::string{"runtime initialization failed: "} + error.what());
    }
}

std::expected<void, std::string> ProxyRuntime::run(std::stop_token stop) {
    if (impl_->started.exchange(true))
        return std::unexpected("runtime already started");
    std::stop_callback canceled{stop, [this] { impl_->stop(); }};
    try {
        if (impl_->config.health.enabled)
            impl_->health = std::jthread([this](std::stop_token token) {
                detail::ServerHealth::run(token, impl_->config.health, [this] {
                    const auto topology = impl_->topology.load();
                    detail::HealthSnapshot snapshot{.routes = {topology, &topology->routes}};
                    for (const auto& server : topology->backends)
                        snapshot.targets.push_back({server.name, server.host, server.port, nativeProtocol(server)});
                    return snapshot;
                });
            });
        if (impl_->config.general.enforceSecureProfile && impl_->config.network.onlineMode)
            impl_->serviceKeys = std::jthread([this] {
                auto keys =
                    impl_->services.servicesKeys
                        ? impl_->services.servicesKeys()
                        : std::make_shared<const wire::auth::ServicesKeySet>(wire::auth::ServicesKeySet::fetch());
                impl_->access.setServices(std::move(keys));
            });
        for (std::size_t i = 0; i < impl_->owners.size(); ++i)
            impl_->threads.emplace_back([this, i] { impl_->owners[i]->run(i == 0); });
    } catch (...) {
        impl_->failed.store(true);
        impl_->stop();
    }
    impl_->threads.clear();
    impl_->health = {};
    if (impl_->workers)
        impl_->workers->join();
    impl_->statusSource = nullptr;
    std::vector<std::uint8_t>{}.swap(impl_->unavailable);
    std::vector<std::uint8_t>{}.swap(impl_->authenticationFailure);
    impl_->bootstrapBuffers.reset();
    if (impl_->failed.load())
        return std::unexpected("runtime stopped after an owner failure");
    return {};
}

std::expected<BackendChangeRequest, BackendChangeError> ProxyRuntime::sendPlayer(std::string_view player,
                                                                                 std::string_view backend) {
    if (!impl_->started.load() || impl_->stopped.load())
        return std::unexpected(BackendChangeError::kNotRunning);
    if (!wire::auth::validLoginName(player))
        return std::unexpected(BackendChangeError::kInvalidPlayer);
    if (!impl_->topology.load()->routes.named(backend))
        return std::unexpected(BackendChangeError::kUnknownBackend);
    auto control = impl_->controls.acquire(1);
    auto storage = impl_->buffers.acquire(1024 + impl_->owners.size() * 128);
    if (!control || !storage)
        return std::unexpected(BackendChangeError::kBusy);
    auto request = std::make_shared<detail::BackendChangeState>(std::move(*control), std::move(*storage));
    request->player = player;
    request->backend = backend;
    request->remaining.store(impl_->owners.size(), std::memory_order_relaxed);
    request->stopped = impl_->controlStopped;
    std::lock_guard publication{request->publication};
    for (const auto& owner : impl_->owners) {
        auto* destination = owner.get();
        if (!destination->mailbox.push([destination, request] {
                {
                    std::lock_guard published{request->publication};
                    if (!request->admitted)
                        return;
                }
                for (auto& [id, session] : destination->sessions) {
                    if (session->playerName() != request->player)
                        continue;
                    if (request->claimed.exchange(true, std::memory_order_relaxed))
                        break;
                    const bool accepted = session->requestBackendChange(request->backend);
                    auto result = accepted ? BackendChangeResult::kAccepted : BackendChangeResult::kUnavailable;
                    if (accepted && !destination->refresh(id, *session)) {
                        destination->close(id);
                        result = BackendChangeResult::kUnavailable;
                    }
                    request->outcome.store(result, std::memory_order_relaxed);
                    break;
                }
                request->remaining.fetch_sub(1, std::memory_order_acq_rel);
            }))
            return std::unexpected(BackendChangeError::kBusy);
        (void)destination->loop.wake();
    }
    request->admitted = true;
    return BackendChangeRequest{std::move(request)};
}

std::expected<std::size_t, std::string> ProxyRuntime::reload(config::ProxyConfig fresh) {
    const auto& current = impl_->config;
    std::string fixed;
    const auto compare = [&fixed](bool same, std::string_view section) {
        if (!same)
            fixed += (fixed.empty() ? "" : ", ") + std::string{section};
    };
    auto liveNetwork = fresh.network;
    liveNetwork.proxyProtocol = current.network.proxyProtocol;
    liveNetwork.proxyProtocolTrusted = current.network.proxyProtocolTrusted;
    liveNetwork.connectionThrottleMs = current.network.connectionThrottleMs;
    liveNetwork.maxConnectionsPerAddress = current.network.maxConnectionsPerAddress;
    liveNetwork.maxJoinsPerTick = current.network.maxJoinsPerTick;
    compare(current.network == liveNetwork, "[network] (bind, port, dual-stack, online-mode, transfers, compression)");
    compare(current.general.enforceSecureProfile == fresh.general.enforceSecureProfile,
            "[general] enforce-secure-profile");
    compare(current.runtime == fresh.runtime, "[runtime]");
    compare(current.admission == fresh.admission, "[admission]");
    compare(current.forwarding == fresh.forwarding, "[forwarding]");
    compare(current.logging == fresh.logging, "[logging]");
    compare(current.bedrock == fresh.bedrock, "[bedrock]");
    compare(current.reload == fresh.reload, "[reload]");
    compare(current.health == fresh.health, "[health] in servers.toml");
    compare(current.statusRelay == fresh.statusRelay, "[status-relay] in servers.toml");
    compare(current.plugins == fresh.plugins, "plugins.toml");
    compare(current.translation == fresh.translation, "[translation]");
    if (!fixed.empty())
        return std::unexpected("changing " + fixed + " requires a restart");
    auto routes = detail::BackendRoutes::create(fresh);
    if (!routes)
        return std::unexpected(routes.error());
    auto forwarding = loadForwarding(fresh);
    if (!forwarding)
        return std::unexpected(forwarding.error());
    if (auto antibot = impl_->access.antibot.apply(fresh.antibot); !antibot)
        return std::unexpected(antibot.error());
    const auto count = fresh.backends.size();
    impl_->access.apply(fresh);
    impl_->topology.store(std::make_shared<const Topology>(Topology{
        std::move(fresh.backends), std::move(fresh.serverGroups), std::move(*routes), std::move(*forwarding)}));
    return count;
}

bool ProxyRuntime::tellPlayer(std::string_view player, std::string_view text) {
    if (!impl_->started.load() || impl_->stopped.load() || !wire::auth::validLoginName(player) || text.empty())
        return false;
    const auto name = std::make_shared<const std::string>(player);
    const auto line = std::make_shared<const std::string>(text.substr(0, 1024));
    bool handed = true;
    for (const auto& owner : impl_->owners) {
        auto* destination = owner.get();
        if (!destination->mailbox.push([destination, name, line] {
                for (auto& [id, session] : destination->sessions) {
                    if (session->playerName() != *name)
                        continue;
                    session->tell(*line);
                    if (!destination->refresh(id, *session))
                        destination->close(id);
                    break;
                }
            }))
            handed = false;
        else
            (void)destination->loop.wake();
    }
    return handed;
}

namespace {
template <typename Apply>
bool deliverToPlayer(auto& impl, std::string_view player, Apply apply) {
    if (!impl.started.load() || impl.stopped.load() || !wire::auth::validLoginName(player))
        return false;
    const auto name = std::make_shared<const std::string>(player);
    bool handed = true;
    for (const auto& owner : impl.owners) {
        auto* destination = owner.get();
        if (!destination->mailbox.push([destination, name, apply] {
                for (auto& [id, session] : destination->sessions) {
                    if (session->playerName() != *name)
                        continue;
                    apply(*session);
                    if (!destination->refresh(id, *session))
                        destination->close(id);
                    break;
                }
            }))
            handed = false;
        else
            (void)destination->loop.wake();
    }
    return handed;
}
} // namespace

bool ProxyRuntime::tellComponent(std::string_view player, wire::text::Component component) {
    const auto shared = std::make_shared<const wire::text::Component>(std::move(component));
    return deliverToPlayer(*impl_, player, [shared](auto& session) { session.tell(*shared); });
}

bool ProxyRuntime::kickComponent(std::string_view player, wire::text::Component component) {
    const auto shared = std::make_shared<const wire::text::Component>(std::move(component));
    return deliverToPlayer(*impl_, player, [shared](auto& session) { session.kick(*shared); });
}

bool ProxyRuntime::sendHud(std::string_view player, PlayerHud action) {
    const auto shared = std::make_shared<const PlayerHud>(std::move(action));
    return deliverToPlayer(*impl_, player, [shared](auto& session) { static_cast<void>(session.applyHud(*shared)); });
}

bool ProxyRuntime::requestCookie(std::string_view player, std::string_view key,
                                 std::function<void(std::optional<std::vector<std::uint8_t>>)> reply) {
    const auto shared = std::make_shared<const std::string>(key);
    const auto callback =
        std::make_shared<std::function<void(std::optional<std::vector<std::uint8_t>>)>>(std::move(reply));
    return deliverToPlayer(*impl_, player, [shared, callback](auto& session) {
        if (!session.requestPluginCookie(*shared, *callback))
            (*callback)(std::nullopt);
    });
}

bool ProxyRuntime::sendPacket(std::string_view player, bool serverbound, std::int32_t phase, std::int32_t id,
                              std::span<const std::uint8_t> body) {
    if (id < 0 || body.size() > impl_->services.packetMaxBytes)
        return false;
    const auto bytes = std::make_shared<const std::vector<std::uint8_t>>(body.begin(), body.end());
    return deliverToPlayer(*impl_, player, [bytes, serverbound, phase, id](auto& session) {
        static_cast<void>(session.pluginPacket(serverbound, phase, id, *bytes));
    });
}

void ProxyRuntime::notifyOperators(std::string_view text) {
    const auto& isOperator = impl_->services.isOperator;
    if (!isOperator)
        return;
    for (const auto& name : playerNames())
        if (isOperator(name))
            static_cast<void>(tellPlayer(name, text));
}

std::string ProxyRuntime::antibotCommand(std::span<const std::string_view> words) {
    auto& engine = impl_->access.antibot;
    const auto now = antibot::Clock::now();
    const std::string_view verb = words.empty() ? std::string_view{"status"} : words[0];
    const std::string_view argument = words.size() > 1 ? words[1] : std::string_view{};
    if (verb == "status" || verb == "stats") {
        const auto stats = engine.stats(now);
        return "Antibot: attack mode " + std::string{stats.attackActive ? "on" : "off"} +
               "; attacks: " + std::to_string(stats.attacks) + "; connections: " + std::to_string(stats.connections) +
               "; refused: " + std::to_string(stats.denied) +
               "; limbo now/entered/passed/failed: " + std::to_string(stats.limboNow) + "/" +
               std::to_string(stats.limboEntered) + "/" + std::to_string(stats.limboPassed) + "/" +
               std::to_string(stats.limboFailed) + "; exploit attempts: " + std::to_string(stats.exploits) +
               "; origins tracked: " + std::to_string(stats.origins) + "; verified: " + std::to_string(stats.verified) +
               ".";
    }
    if (verb == "attack" && (argument == "on" || argument == "off" || argument == "auto")) {
        engine.forceAttack(argument == "on" ? 1 : argument == "off" ? -1 : 0);
        return "Attack mode is now " + std::string{argument == "auto" ? "automatic" : argument} + ".";
    }
    if ((verb == "ban" || verb == "unban") && !argument.empty()) {
        if (!config::AddressRange::parseAddress(argument))
            return "Not an IP address.";
        const auto origin = antibot::originOf(argument);
        if (verb == "ban") {
            engine.ban(origin, now);
            return "Origin banned.";
        }
        return engine.unban(origin) ? "Origin cleared." : "That origin is not tracked.";
    }
    if ((verb == "verify" || verb == "unverify" || verb == "verified") && wire::auth::validLoginName(argument)) {
        if (verb == "verify")
            antibotVerify(argument);
        else if (verb == "unverify")
            antibotUnverify(argument);
        else
            return std::string{argument} + (antibotVerified(argument) ? " is verified." : " is not verified.");
        return "Done.";
    }
    return "Usage: antibot status | attack on|off|auto | ban <ip> | unban <ip> | verify|unverify|verified <name>.";
}

void ProxyRuntime::antibotVerify(std::string_view player) {
    impl_->access.antibot.verify(player, std::nullopt, antibot::Clock::now());
}

void ProxyRuntime::antibotUnverify(std::string_view player) { impl_->access.antibot.unverify(player); }

bool ProxyRuntime::antibotVerified(std::string_view player) const {
    return impl_->access.antibot.isVerified(player, std::nullopt, antibot::Clock::now());
}

namespace {
bool bedrockName(std::string_view name) {
    return !name.empty() && name.size() <= 16 &&
           std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) != 0 || c == ' ' || c == '_'; });
}
} // namespace

int ProxyRuntime::bedrockPacketsPerSecond() const { return impl_->access.antibot.config()->bedrockPacketsPerSecond; }

int ProxyRuntime::statusCacheMs() const {
    const auto config = impl_->access.antibot.config();
    if (!config->enabled || config->statusCacheMs <= 0)
        return 0;
    return !config->statusCacheOnlyInAttack || antibotAttack() ? config->statusCacheMs : 0;
}

int ProxyRuntime::bedrockLoginSeconds() const { return impl_->access.antibot.config()->bedrockLoginSeconds; }

bool ProxyRuntime::bedrockOffline(std::string_view address, bool connecting) {
    auto& engine = impl_->access.antibot;
    const auto config = engine.config();
    if (!config->enabled || !config->bedrockEnabled)
        return true;
    const auto origin = antibot::originOf(address);
    const auto now = antibot::Clock::now();
    if (!connecting)
        return engine.statusAllowed(origin, now);
    if (config->bedrockConnectScore > 0)
        engine.penalize(origin, config->bedrockConnectScore, now);
    const auto verdict = engine.connection(origin, now);
    if (verdict.action == antibot::Action::kDeny)
        return false;
    if (verdict.score >= config->banScore) {
        engine.ban(origin, now);
        return false;
    }
    return true;
}

std::optional<std::string> ProxyRuntime::bedrockLogin(std::string_view address, std::string_view name,
                                                      std::string_view xuid, bool authenticated) {
    auto& engine = impl_->access.antibot;
    const auto config = engine.config();
    if (!config->enabled || !config->bedrockEnabled)
        return std::nullopt;
    const auto origin = antibot::originOf(address);
    const auto now = antibot::Clock::now();
    const bool shape =
        bedrockName(name) &&
        (!authenticated || (!xuid.empty() && xuid.size() <= 20 &&
                            std::ranges::all_of(xuid, [](unsigned char c) { return std::isdigit(c) != 0; })));
    if (!shape) {
        engine.loginFailed(origin, now);
        return std::string{"Your login could not be verified."};
    }
    std::string javaLike{name};
    std::ranges::replace(javaLike, ' ', '_');
    const auto verdict = engine.login(origin, javaLike, false, now);
    if (verdict.action == antibot::Action::kDeny) {
        engine.loginFailed(origin, now);
        return std::string{"Too many connection attempts, try again later."};
    }
    const bool strict =
        verdict.action != antibot::Action::kAllow || (config->bedrockXboxInAttack && engine.attackActive(now));
    if (strict && !authenticated)
        return std::string{"Sign in to Xbox Live to play here."};
    return std::nullopt;
}

void ProxyRuntime::bedrockFailed(std::string_view address) {
    impl_->access.antibot.loginFailed(antibot::originOf(address), antibot::Clock::now());
}

bool ProxyRuntime::antibotAttack() const { return impl_->access.antibot.attackActive(antibot::Clock::now()); }

bool ProxyRuntime::kickPlayer(std::string_view player, std::string_view reason) {
    if (!impl_->started.load() || impl_->stopped.load() || !wire::auth::validLoginName(player))
        return false;
    const auto name = std::make_shared<const std::string>(player);
    const auto text = std::make_shared<const std::string>(reason.substr(0, 1024));
    bool handed = true;
    for (const auto& owner : impl_->owners) {
        auto* destination = owner.get();
        if (!destination->mailbox.push([destination, name, text] {
                for (auto& [id, session] : destination->sessions) {
                    if (session->playerName() != *name)
                        continue;
                    session->kick(*text);
                    if (!destination->refresh(id, *session))
                        destination->close(id);
                    break;
                }
            }))
            handed = false;
        else
            (void)destination->loop.wake();
    }
    return handed;
}

std::vector<std::string> ProxyRuntime::playerNames() const {
    std::lock_guard lock{impl_->namesMutex};
    return impl_->names;
}

void ProxyRuntime::resolveDecision(std::string_view player, std::uint64_t token, DecisionResult result) {
    static_cast<void>(player);
    if (!impl_->started.load() || impl_->stopped.load())
        return;
    const auto answer = std::make_shared<const DecisionResult>(std::move(result));
    for (const auto& owner : impl_->owners) {
        auto* destination = owner.get();
        if (destination->mailbox.push([destination, token, answer] {
                for (auto& [id, session] : destination->sessions) {
                    if (!session->awaiting(token))
                        continue;
                    if (!session->decided(token, *answer) || !destination->refresh(id, *session))
                        destination->close(id);
                    break;
                }
            }))
            (void)destination->loop.wake();
    }
}

bool ProxyRuntime::sendPluginMessage(std::string_view player, std::string_view channel,
                                     std::span<const std::uint8_t> data, bool towardBackend) {
    if (!impl_->started.load() || impl_->stopped.load() || !wire::auth::validLoginName(player) || channel.empty() ||
        data.size() > 32768)
        return false;
    const auto name = std::make_shared<const std::string>(player);
    const auto where = std::make_shared<const std::string>(channel);
    const auto bytes = std::make_shared<const std::vector<std::uint8_t>>(data.begin(), data.end());
    bool handed = true;
    for (const auto& owner : impl_->owners) {
        auto* destination = owner.get();
        if (!destination->mailbox.push([destination, name, where, bytes, towardBackend] {
                for (auto& [id, session] : destination->sessions) {
                    if (session->playerName() != *name)
                        continue;
                    session->sendPluginMessage(*where, *bytes, towardBackend);
                    if (!destination->refresh(id, *session))
                        destination->close(id);
                    break;
                }
            }))
            handed = false;
        else
            (void)destination->loop.wake();
    }
    return handed;
}

std::vector<std::string> ProxyRuntime::backendNames() const {
    std::vector<std::string> names;
    for (const auto& backend : impl_->topology.load()->backends)
        names.push_back(backend.name);
    return names;
}

std::vector<BackendInfo> ProxyRuntime::backends() const {
    std::vector<BackendInfo> result;
    const auto topology = impl_->topology.load();
    for (std::size_t i = 0; i < topology->backends.size(); ++i) {
        const auto& backend = topology->backends[i];
        result.push_back({backend.name, backend.host, backend.port, backend.protocol, backend.automatic,
                          std::string{wire::auth::forwarding::forwardingModeName(backend.forwarding)},
                          topology->routes.reachable(i)});
    }
    return result;
}

std::vector<ServerGroupInfo> ProxyRuntime::serverGroups() const {
    std::vector<ServerGroupInfo> result;
    const auto topology = impl_->topology.load();
    for (const auto& group : topology->groups) {
        ServerGroupInfo info{group.name, {}};
        for (const auto& name : group.servers)
            if (const auto index = topology->routes.named(name))
                info.servers.push_back({name, topology->routes.reachable(*index)});
        result.push_back(std::move(info));
    }
    return result;
}

std::optional<wire::protocol::status::ServerStatusPayload> ProxyRuntime::relayStatus(std::string_view source,
                                                                                     std::string_view host) const {
    const auto topology = impl_->topology.load();
    std::optional<std::size_t> index;
    if (source.starts_with("server:")) {
        index = topology->routes.named(source.substr(7));
    } else if (source == "route") {
        for (const auto candidate : topology->routes.select(host))
            if (topology->routes.reachable(candidate)) {
                index = candidate;
                break;
            }
    }
    return index && impl_->relay ? impl_->relay->get(topology->backends[*index].name) : std::nullopt;
}

std::expected<std::vector<SessionInfo>, std::string> ProxyRuntime::sessions(std::chrono::milliseconds timeout) const {
    if (!impl_->started.load() || impl_->stopped.load())
        return std::unexpected("the proxy is not running");

    struct Collected {
        std::mutex mutex;
        std::condition_variable done;
        std::vector<SessionInfo> sessions;
        std::size_t remaining = 0;
    };

    static constexpr std::array<std::string_view, 4> kPhases{"handshake", "login", "configuration", "play"};
    const auto collected = std::make_shared<Collected>();
    collected->remaining = impl_->owners.size();
    for (const auto& owner : impl_->owners) {
        auto* destination = owner.get();
        if (!destination->mailbox.push([destination, collected] {
                std::vector<SessionInfo> mine;
                for (const auto& [id, session] : destination->sessions)
                    mine.push_back({std::string{session->playerName()}, session->playerUuid(),
                                    std::string{session->activeBackendName()},
                                    session->clientProtocol() == 0 ? buildInfo::kProtocolVersion
                                                                   : session->clientProtocol(),
                                    std::string{kPhases[session->phase()]}});
                const std::lock_guard lock{collected->mutex};
                std::ranges::move(mine, std::back_inserter(collected->sessions));
                --collected->remaining;
                collected->done.notify_all();
            }))
            return std::unexpected("the control queue is full");
        (void)destination->loop.wake();
    }
    std::unique_lock lock{collected->mutex};
    if (!collected->done.wait_for(lock, timeout, [&collected] { return collected->remaining == 0; }))
        return std::unexpected("the session owners did not answer in time");
    return std::move(collected->sessions);
}

std::optional<std::pair<std::string, std::uint16_t>> ProxyRuntime::backendEndpoint(std::string_view name) const {
    for (const auto& backend : impl_->topology.load()->backends)
        if (backend.name == name)
            return std::pair{backend.host, backend.port};
    return std::nullopt;
}

std::uint16_t ProxyRuntime::port() const noexcept { return impl_->port; }

std::uint64_t ProxyRuntime::acceptedConnections() const noexcept {
    return impl_->accepted.load(std::memory_order_relaxed);
}

std::uint64_t ProxyRuntime::bytesTowardClients() const noexcept {
    return detail::relayCounters().towardClients.load(std::memory_order_relaxed);
}

std::uint64_t ProxyRuntime::bytesTowardBackends() const noexcept {
    return detail::relayCounters().towardBackends.load(std::memory_order_relaxed);
}

std::uint64_t ProxyRuntime::rejectedConnections() const noexcept {
    return impl_->rejected.load(std::memory_order_relaxed);
}

std::size_t ProxyRuntime::sessionCount() const noexcept { return impl_->slots.used(); }

std::size_t ProxyRuntime::bufferedBytes() const noexcept { return impl_->buffers.used(); }

} // namespace basalt::proxy::runtime
