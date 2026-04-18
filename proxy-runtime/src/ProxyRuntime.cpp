#include "ClientSession.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/Admission.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/io/EventLoop.hpp>
#include <basalt/wire/io/ResourceBudget.hpp>
#include <basalt/wire/io/TaskQueue.hpp>
#include <basalt/wire/io/TimerQueue.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>
#include <basalt/wire/text/Component.hpp>

#include <atomic>
#include <exception>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace basalt::proxy::runtime {
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

        void close(std::uint64_t id) {
            const auto found = sessions.find(id);
            if (found == sessions.end())
                return;
            loop.unwatch(found->second->handle());
            (void)timers.cancel(found->second->timer);
            sessions.erase(found);
        }

        void add(std::shared_ptr<Incoming> incoming) {
            incoming->handoff.reset();
            if (runtime.stopped.load() ||
                Clock::now() >=
                    incoming->acceptedAt + std::chrono::milliseconds{runtime.config.runtime.handshakeTimeoutMs})
                return;
            auto session = std::make_unique<Session>(std::move(incoming->connection), std::move(incoming->slot),
                                                     std::move(incoming->buffers), runtime.config.runtime,
                                                     runtime.status, runtime.unavailable, incoming->acceptedAt);
            if (!session->accounted())
                return;
            const auto id = runtime.nextId.fetch_add(1);
            if (id == 0)
                throw std::runtime_error("session identity exhausted");
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
                if (runtime.admission.attempt(connection->remoteAddress(), now) != AdmissionResult::Accepted)
                    continue;
                auto slot = runtime.slots.acquire(1);
                auto allocation = runtime.buffers.acquire(Session::kBufferBytes);
                auto handoff = runtime.handoffs.acquire(1);
                if (!slot || !allocation || !handoff || !connection->setNonBlocking())
                    continue;
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
                    for (const auto& timer : timers.expire(now))
                        close(timer.token);
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
                        if (!session.ready(event) || !timers.reschedule(session.timer, session.deadline()) ||
                            !loop.watch(session.handle(), session.interest(), event.token))
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
    Admission admission;
    ResourceBudget slots;
    ResourceBudget buffers;
    ResourceBudget handoffs;
    std::optional<ResourceBudget::Reservation> bootstrapBuffers;
    wire::io::TcpListener listener;
    std::uint16_t port = 0;
    std::vector<std::uint8_t> status;
    std::vector<std::uint8_t> unavailable;
    std::vector<std::unique_ptr<Owner>> owners;
    std::vector<std::jthread> threads;
    std::atomic<bool> stopped{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> started{false};
    std::atomic<std::uint64_t> nextId{1};
    std::size_t nextOwner = 0;

    explicit Impl(config::ProxyConfig settings)
        : config(std::move(settings)), admission(config.admission), slots(config.runtime.maxSessions),
          buffers(config.runtime.maxBufferedBytes), handoffs(config.runtime.maxPendingWork) {}

    void stop() {
        stopped.store(true);
        for (const auto& owner : owners)
            (void)owner->loop.wake();
    }

    ~Impl() {
        stop();
        threads.clear();
    }
};

ProxyRuntime::ProxyRuntime(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ProxyRuntime::ProxyRuntime(ProxyRuntime&&) noexcept = default;
ProxyRuntime& ProxyRuntime::operator=(ProxyRuntime&&) noexcept = default;

ProxyRuntime::~ProxyRuntime() = default;

std::expected<ProxyRuntime, std::string> ProxyRuntime::create(config::ProxyConfig config) {
    const auto& limits = config.runtime;
    if (limits.ioLoops == 0 || limits.ioLoops > 64 || limits.maxSessions == 0 || limits.maxSessions > 65536 ||
        limits.maxPendingWork == 0 || limits.maxPendingWork > 65536 ||
        limits.sessionBufferBytes < Session::kBufferBytes || limits.maxBufferedBytes < Session::kBufferBytes ||
        limits.handshakeTimeoutMs == 0 || limits.readTimeoutMs == 0 || limits.writeTimeoutMs == 0 ||
        limits.lowWaterBytes >= limits.highWaterBytes || limits.highWaterBytes >= limits.sessionBufferBytes)
        return std::unexpected("invalid runtime resource limits");
    auto address = wire::io::TcpAddress::literal(config.network.bind, config.network.port);
    if (!address)
        return std::unexpected("invalid listener address");
    if (config.network.dualStack && config.network.bind.find(':') == std::string::npos)
        return std::unexpected("dual-stack requires an IPv6 listener");
    try {
        auto impl = std::make_unique<Impl>(std::move(config));
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
            wire::text::Component::text("Player routing is unavailable in this build.")}
            .write(packet);
        if (!protocol::packetFraming::writeUncompressed(frame, packet.readable()))
            return std::unexpected("login rejection encoding failed");
        impl->unavailable = frame.toVector();
        if (impl->status.size() + 10 > Session::kBufferCapacity || impl->unavailable.size() > Session::kBufferCapacity)
            return std::unexpected("bootstrap responses exceed buffer limits");
        impl->bootstrapBuffers = impl->buffers.acquire(impl->status.capacity() + impl->unavailable.capacity());
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
    std::vector<std::uint8_t>{}.swap(impl_->status);
    std::vector<std::uint8_t>{}.swap(impl_->unavailable);
    impl_->bootstrapBuffers.reset();
    if (impl_->failed.load())
        return std::unexpected("runtime stopped after an owner failure");
    return {};
}

std::uint16_t ProxyRuntime::port() const noexcept { return impl_->port; }

std::size_t ProxyRuntime::sessionCount() const noexcept { return impl_->slots.used(); }

std::size_t ProxyRuntime::bufferedBytes() const noexcept { return impl_->buffers.used(); }

} // namespace basalt::proxy::runtime
