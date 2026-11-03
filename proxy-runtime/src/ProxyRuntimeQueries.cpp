#include "ProxyRuntimeImpl.hpp"

namespace basalt::proxy::runtime {
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

std::optional<config::BackendConfig> ProxyRuntime::backendConfig(std::string_view name) const {
    const auto topology = impl_->topology.load();
    const auto found = std::ranges::find(topology->backends, name, &config::BackendConfig::name);
    if (found == topology->backends.end())
        return std::nullopt;
    return *found;
}

std::optional<config::BackendConfig> ProxyRuntime::defaultBackend() const {
    const auto topology = impl_->topology.load();
    const auto defaults = topology->routes.select("bedrock.invalid");
    if (defaults.empty())
        return std::nullopt;
    return topology->backends[defaults.front()];
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
                    mine.push_back(
                        {std::string{session->playerName()}, session->playerUuid(),
                         std::string{session->activeBackendName()},
                         session->clientProtocol() == 0 ? buildInfo::kProtocolVersion : session->clientProtocol(),
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
