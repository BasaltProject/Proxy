#pragma once

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
#include <basalt/wire/config/DefaultIcon.hpp>
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
#include <basalt/wire/text/Serializers.hpp>

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
namespace impl {
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

inline std::int32_t nativeProtocol(const config::BackendConfig& server) {
    return server.protocol == config::BackendConfig::kClientProtocol ? buildInfo::kProtocolVersion : server.protocol;
}

inline std::expected<std::shared_ptr<const detail::NativeForwarding>, std::string>
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
} // namespace impl

using namespace impl;

namespace impl {
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
} // namespace impl

using namespace impl;

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
            if (runtime.services.pluginChannels)
                session->usePluginChannels(runtime.services.pluginChannels);
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
                    if (runtime.admission.attempt(
                            connection->remoteAddress(), now,
                            runtime.access.antibot.knownOrigin(antibot::originOf(connection->remoteAddress()), now)) !=
                        AdmissionResult::Accepted) {
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
    std::atomic<std::int64_t> bedrockSecond{-1};
    std::atomic<std::uint32_t> bedrockOpened{0};
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

} // namespace basalt::proxy::runtime
