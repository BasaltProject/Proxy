#include "BackendChangeState.hpp"
#include "BackendRoutes.hpp"
#include "ClientSession.hpp"
#include "ForwardingKey.hpp"
#include "NativeForwarding.hpp"
#include "RelayCounters.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/Admission.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/auth/SessionKeyPair.hpp>
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

#include <algorithm>
#include <atomic>
#include <exception>
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
    detail::BackendRoutes routes;
    std::shared_ptr<const detail::NativeForwarding> forwarding;
};

/// libc++ has no std::atomic<std::shared_ptr>, so a mutex guards the pointer swap; readers copy it and release the
/// lock.
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
            if (found->second->announced && runtime.services.playerEvents)
                runtime.services.playerEvents({PlayerEventKind::kLeave,
                                               std::string{found->second->playerName()},
                                               found->second->announcedBackend,
                                               found->second->playerUuid(),
                                               {}});
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
                runtime.config.runtime, runtime.status, runtime.unavailable, incoming->acceptedAt,
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
                runtime.services.playerLogin);
            if (!session->accounted())
                return;
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
            if (!runtime.services.playerEvents || !session.inPlay())
                return;
            const auto backend = session.activeBackendName();
            if (!session.announced) {
                session.announced = true;
                session.announcedBackend = backend;
                runtime.services.playerEvents({PlayerEventKind::kJoin,
                                               std::string{session.playerName()},
                                               session.announcedBackend,
                                               session.playerUuid(),
                                               {}});
            } else if (backend != session.announcedBackend) {
                const std::string from = session.announcedBackend;
                session.announcedBackend = backend;
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
                if (runtime.admission.attempt(connection->remoteAddress(), now) != AdmissionResult::Accepted) {
                    runtime.rejected.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                auto slot = runtime.slots.acquire(1);
                auto allocation = runtime.buffers.acquire(Session::kBufferBytes);
                auto handoff = runtime.handoffs.acquire(1);
                if (!slot || !allocation || !handoff || !connection->setNonBlocking()) {
                    runtime.rejected.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                runtime.accepted.fetch_add(1, std::memory_order_relaxed);
                auto incoming = std::make_shared<Incoming>(Incoming{std::move(*slot), std::move(*allocation),
                                                                    std::move(*handoff), std::move(*connection), now});
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
                            loop.unwatch(event.handle);
                            healthy = session.backendReady(event);
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
    ResourceBudget slots;
    ResourceBudget buffers;
    ResourceBudget handoffs;
    ResourceBudget controls;
    std::optional<ResourceBudget::Reservation> bootstrapBuffers;
    wire::io::TcpListener listener;
    std::uint16_t port = 0;
    std::vector<std::uint8_t> status;
    std::vector<std::uint8_t> unavailable;
    std::vector<std::uint8_t> authenticationFailure;
    RuntimeServices services;
    std::optional<wire::auth::SessionKeyPair> key;
    std::optional<wire::io::WorkerPool> workers;
    detail::IdentityRegistry identities;
    std::vector<std::unique_ptr<Owner>> owners;
    std::vector<std::jthread> threads;
    std::atomic<bool> stopped{false};
    std::shared_ptr<std::atomic<bool>> controlStopped = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> failed{false};
    std::atomic<bool> started{false};
    std::atomic<std::uint64_t> nextId{1};
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::size_t nextOwner = 0;

    explicit Impl(config::ProxyConfig settings)
        : config(std::move(settings)), admission(config.admission), slots(config.runtime.maxSessions),
          buffers(config.runtime.maxBufferedBytes), handoffs(config.runtime.maxPendingWork),
          controls(config.runtime.maxPendingWork), identities(config.runtime.maxSessions) {}

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
            Topology{impl->config.backends, std::move(*routes), std::move(*forwarding)}));
        impl->services = std::move(services);
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
        protocol::status::ServerStatusPayload status{
            .description = wire::text::Component::text("Basalt Proxy"),
            .players =
                protocol::status::ServerStatusPayload::Players{
                    .max = static_cast<std::int32_t>(impl->config.runtime.maxSessions), .online = 0, .sample = {}},
            .version = protocol::status::ServerStatusPayload::Version{std::string{buildInfo::kVersion},
                                                                      buildInfo::kProtocolVersion},
            .favicon = std::nullopt};
        protocol::varInt::write(packet, protocol::status::StatusResponsePacket::kId);
        protocol::status::StatusResponsePacket{std::move(status)}.write(packet);
        if (!protocol::packetFraming::writeUncompressed(frame, packet.readable()))
            return std::unexpected("status encoding failed");
        impl->status = frame.toVector();
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
        if (impl->status.size() + 10 > Session::kBufferCapacity || impl->unavailable.size() > Session::kBufferCapacity)
            return std::unexpected("bootstrap responses exceed buffer limits");
        impl->bootstrapBuffers = impl->buffers.acquire(impl->status.capacity() + impl->unavailable.capacity() +
                                                       impl->authenticationFailure.capacity());
        if (!impl->bootstrapBuffers || impl->buffers.limit() - impl->buffers.used() < Session::kBufferBytes)
            return std::unexpected("global buffer budget cannot admit a bootstrap session");
        impl->owners.reserve(impl->config.runtime.ioLoops);
        impl->threads.reserve(impl->config.runtime.ioLoops);
        for (std::size_t i = 0; i < impl->config.runtime.ioLoops; ++i)
            impl->owners.push_back(std::make_unique<Impl::Owner>(*impl));
        auto bound = wire::io::TcpListener::bind(*address, 128, impl->config.network.dualStack);
        if (!bound || !bound->setNonBlocking())
            return std::unexpected("could not bind the public listener");
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
        for (std::size_t i = 0; i < impl_->owners.size(); ++i)
            impl_->threads.emplace_back([this, i] { impl_->owners[i]->run(i == 0); });
    } catch (...) {
        impl_->failed.store(true);
        impl_->stop();
    }
    impl_->threads.clear();
    if (impl_->workers)
        impl_->workers->join();
    std::vector<std::uint8_t>{}.swap(impl_->status);
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
    compare(current.network == fresh.network, "[network]");
    compare(current.runtime == fresh.runtime, "[runtime]");
    compare(current.admission == fresh.admission, "[admission]");
    compare(current.forwarding == fresh.forwarding, "[forwarding]");
    compare(current.logging == fresh.logging, "[logging]");
    compare(current.bedrock == fresh.bedrock, "[bedrock]");
    compare(current.reload == fresh.reload, "[reload]");
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
    const auto count = fresh.backends.size();
    impl_->topology.store(std::make_shared<const Topology>(
        Topology{std::move(fresh.backends), std::move(*routes), std::move(*forwarding)}));
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

std::vector<std::string> ProxyRuntime::backendNames() const {
    std::vector<std::string> names;
    for (const auto& backend : impl_->topology.load()->backends)
        names.push_back(backend.name);
    return names;
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
