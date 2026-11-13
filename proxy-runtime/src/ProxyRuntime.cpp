#include "ProxyRuntimeImpl.hpp"

namespace basalt::proxy::runtime {
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
        if (impl->services.verifiedSave) {
            impl->access.antibot.persistVerified(
                {impl->services.verifiedSave, impl->services.verifiedErase, impl->services.verifiedPurge});
            if (impl->services.verifiedLoad)
                for (const auto& row : impl->services.verifiedLoad(impl->access.antibot.config()->verifiedMax))
                    impl->access.antibot.restoreVerified(row.name, row.origin, row.expiresUnixSeconds);
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
            const auto icon = wire::config::defaultIcon::bytes();
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
                          .favicon = std::vector<std::uint8_t>{icon.begin(), icon.end()}};
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
            impl_->serviceKeys = std::jthread([impl = impl_.get()] {
                auto keys =
                    impl->services.servicesKeys
                        ? impl->services.servicesKeys()
                        : std::make_shared<const wire::auth::ServicesKeySet>(wire::auth::ServicesKeySet::fetch());
                impl->access.setServices(std::move(keys));
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
} // namespace basalt::proxy::runtime
