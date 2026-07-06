#include "BedrockStatus.hpp"

#include <basalt/wire/bedrock/BedrockPacketIds.hpp>
#include <basalt/wire/bedrock/NetherNetIdentityStore.hpp>
#include <basalt/wire/common/logging/Logger.hpp>

#include <algorithm>
#include <chrono>

namespace basalt::proxy::runtime::detail {
namespace {

constexpr std::string_view kGameVersion = wire::bedrock::kBedrockGameVersion;
constexpr int kPollTimeoutMillis = 5;
constexpr std::size_t kMaxHandshakes = 64;
constexpr std::size_t kMaxOrders = 1024;
constexpr std::uint64_t kNetherNetStatusRefreshMillis = 1000;
constexpr std::string_view kNetherNetIdentityFile = ".basalt/nethernet-identity.der";
constexpr std::string_view kTurnedAway = "Bedrock players cannot join this network yet.";

std::uint64_t nowMillis() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string originOf(const wire::io::UdpEndpoint& peer) {
    const std::uint32_t address = peer.address;
    return std::to_string(address & 0xff) + "." + std::to_string((address >> 8) & 0xff) + "." +
           std::to_string((address >> 16) & 0xff) + "." + std::to_string(address >> 24);
}

std::string cleanLine(std::string_view text) {
    std::string clean;
    for (const char character : text)
        clean += (character == ';' || character == '\n' || character == '\r') ? ' ' : character;
    return clean;
}

} // namespace

BedrockStatus::BedrockStatus(const config::BedrockConfig& options, std::function<std::string()> motd,
                             std::function<std::size_t()> online, std::size_t max, Route route,
                             BedrockSession::Events events, std::shared_ptr<const wire::text::LangTable> language,
                             Lookup lookup, std::shared_ptr<const NativeForwarding> forwarding,
                             bool requireAuthenticated, const config::AdmissionConfig& admission,
                             BedrockSession::Commands commands)
    : options_(options), port_(options.port), motd_(std::move(motd)), online_(std::move(online)), max_(max),
      route_(std::move(route)), events_(std::move(events)), language_(std::move(language)), lookup_(std::move(lookup)),
      forwarding_(std::move(forwarding)), requireAuthenticated_(requireAuthenticated), admission_(admission),
      commands_(std::move(commands)) {
    commands_.connect = [this](std::string_view player, std::string_view backend) { return connect(player, backend); };
}

BedrockStatus::~BedrockStatus() { stop(); }

std::string BedrockStatus::advertisement() const {
    std::string line = "MCPE;" + (motd_ ? motd_() : std::string{"Basalt Proxy"}) + ";";
    line += std::to_string(wire::bedrock::kNativeBedrockProtocol) + ";";
    line += std::string{kGameVersion} + ";";
    const auto playing = static_cast<std::size_t>(
        std::ranges::count_if(clients_, [](const auto& entry) { return entry.second.session != nullptr; }));
    line += std::to_string((online_ ? online_() : 0) + playing) + ";";
    line += std::to_string(max_) + ";";
    line += std::to_string(server_ ? server_->guid() : 0) + ";";
    line += subMotd_ + ";Survival;1;";
    line += std::to_string(port()) + ";" + std::to_string(port()) + ";";
    return line;
}

bool BedrockStatus::start() {
    auto keys = wire::bedrock::EcKeyPair::generate();
    if (!keys)
        return false;
    keys_ = std::make_shared<const wire::bedrock::EcKeyPair>(std::move(*keys));
    const wire::common::Logger logger{"Bedrock"};
    if (options_.transport != config::BedrockTransport::NetherNet) {
        wire::bedrock::RakServerConfig config;
        config.port = port_;
        config.guid = nowMillis() ^ 0x6261736c61747870ULL;
        config.maxSessions = std::max(kMaxHandshakes, max_);
        server_ = std::make_unique<wire::bedrock::RakServer>(config);
        if (!server_->start()) {
            logger.warn([this] { return "Could not bind UDP port " + std::to_string(port_) + " for RakNet."; });
            server_.reset();
        } else {
            server_->onAdvertise([this] { return advertisement(); });
            server_->onOffline([this](const wire::io::UdpEndpoint& peer, bool connecting) {
                return !guard_.offline || guard_.offline(originOf(peer), connecting);
            });
            server_->onConnect([this](wire::bedrock::RakSession& session) { adopt(session, session.peer()); });
            server_->onMessage([this](wire::bedrock::RakSession& session, std::span<const std::uint8_t> payload) {
                if (const auto found = clients_.find(session.peer()); found != clients_.end())
                    found->second.connection->receive(payload);
            });
            server_->onDisconnect([this](wire::bedrock::RakSession& session) { release(session.peer()); });
        }
    }
    if (options_.transport != config::BedrockTransport::RakNet && !startNetherNet())
        nether_.reset();
    if (server_ == nullptr && nether_ == nullptr)
        return false;
    running_.store(true);
    thread_ = std::jthread{[this] {
        std::uint64_t statusAt = 0;
        while (running_.load()) {
            if (server_) {
                server_->poll(nowMillis(), kPollTimeoutMillis);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds{kPollTimeoutMillis});
            }
            if (nether_) {
                nether_->drain();
                if (const std::uint64_t now = nowMillis(); now - statusAt >= kNetherNetStatusRefreshMillis) {
                    statusAt = now;
                    refreshNetherNetStatus();
                }
            }
            std::vector<Order> orders;
            {
                const std::lock_guard lock{orders_};
                orders.swap(pending_);
            }
            for (const Order& order : orders) {
                for (auto& [peer, client] : clients_) {
                    if (!client.session || client.session->name() != order.player)
                        continue;
                    if (order.kind == Order::Kind::kKick) {
                        client.session->kick(order.text);
                    } else if (order.kind == Order::Kind::kTell) {
                        client.session->tell(order.text);
                    } else if (const auto target = lookup_ ? lookup_(order.text) : std::nullopt; target) {
                        wire::common::Logger{"Bedrock"}.info(
                            [&] { return "Moving " + order.player + " to " + target->name + "."; });
                        if (!client.session->switchTo(*target)) {
                            wire::common::Logger{"Bedrock"}.warn(
                                [&] { return "Could not move " + order.player + " to " + target->name + "."; });
                            client.session->kick("The server could not be reached.");
                        }
                    }
                }
            }
            const auto limit = guard_.loginSeconds ? guard_.loginSeconds() : 0;
            const auto now = std::chrono::steady_clock::now();
            for (auto& [peer, client] : clients_) {
                if (client.session) {
                    client.session->pump();
                } else if (limit > 0 && !client.stalled && now - client.since > std::chrono::seconds{limit}) {
                    client.stalled = true;
                    if (guard_.failed)
                        guard_.failed(originOf(peer));
                    client.connection->disconnect("Login took too long.");
                }
            }
        }
    }};
    return true;
}

void BedrockStatus::adopt(wire::bedrock::BedrockTransport& transport, const wire::io::UdpEndpoint& peer,
                          std::shared_ptr<wire::bedrock::NetherNetLink> link) {
    if (admission_.attempt(originOf(peer)) != AdmissionResult::Accepted) {
        if (link)
            link->close();
        else if (server_)
            static_cast<wire::bedrock::RakSession&>(transport).close();
        return;
    }
    auto connection = std::make_unique<wire::bedrock::BedrockConnection>(transport, keys_, std::string{kGameVersion});
    wire::bedrock::BedrockConnection& raw = *connection;
    raw.onLogin([](const wire::bedrock::LoginRequest& login) {
        wire::common::Logger{"Bedrock"}.info([&login] {
            return login.name + " is joining from Bedrock" + (login.authenticated ? "" : " (unauthenticated)") + ".";
        });
    });
    raw.onReady([this, &raw, peer] {
        if (guard_.login) {
            const auto login = raw.login();
            if (const auto refusal = guard_.login(originOf(peer), login.name, login.xuid, login.authenticated)) {
                raw.disconnect(*refusal);
                return;
            }
        }
        if (requireAuthenticated_ && !raw.login().authenticated) {
            wire::common::Logger{"Bedrock"}.info([&raw] {
                return raw.login().name + " was turned away: this proxy only lets in players signed in to Xbox Live.";
            });
            raw.disconnect("Sign in to Xbox Live to play here.");
            return;
        }
        const auto found = clients_.find(peer);
        const auto backend = route_ ? route_() : std::nullopt;
        if (found == clients_.end() || !backend) {
            wire::common::Logger{"Bedrock"}.warn([] { return std::string{"No server to send a Bedrock player to."}; });
            raw.disconnect(kTurnedAway);
            return;
        }
        found->second.session = std::make_unique<BedrockSession>(raw, *backend, originOf(peer), peer.port, events_,
                                                                 language_, forwarding_, commands_);
        if (found->second.session->begin()) {
            const std::lock_guard lock{orders_};
            playing_.insert(found->second.session->name());
        } else if (found->second.session->turnedAway()) {
            found->second.session.reset();
        } else {
            wire::common::Logger{"Bedrock"}.warn([] { return std::string{"The server could not be resolved."}; });
            found->second.session.reset();
            raw.disconnect("The server could not be reached.");
        }
    });
    raw.onPacket([this, peer](wire::bedrock::BedrockPacket packet, wire::io::ByteBuffer& body) {
        const auto found = clients_.find(peer);
        if (found == clients_.end())
            return;
        if (guard_.packetsPerSecond) {
            const auto limit = guard_.packetsPerSecond();
            const auto now = std::chrono::steady_clock::now();
            if (now - found->second.windowStart >= std::chrono::seconds{1}) {
                found->second.windowStart = now;
                found->second.packets = 0;
            }
            if (limit > 0 && ++found->second.packets > static_cast<std::uint32_t>(limit)) {
                if (guard_.failed)
                    guard_.failed(originOf(peer));
                found->second.connection->disconnect("Too many packets.");
                return;
            }
        }
        if (found->second.session)
            found->second.session->fromBedrock(packet, body);
    });
    if (link && !link->identityKey().empty())
        raw.expectIdentity(link->identityKey());
    clients_.insert_or_assign(peer, Client{std::move(connection), nullptr, std::move(link)});
}

void BedrockStatus::release(const wire::io::UdpEndpoint& peer) {
    if (const auto found = clients_.find(peer); found != clients_.end() && found->second.session) {
        const std::lock_guard lock{orders_};
        playing_.erase(found->second.session->name());
    }
    clients_.erase(peer);
}

bool BedrockStatus::startNetherNet() {
    const wire::common::Logger logger{"Bedrock"};
    wire::bedrock::NetherNetConfig config;
    config.signalingPort = port_;
    if (options_.nethernetPort != 0)
        config.icePort = options_.nethernetPort;
    else if (server_)
        config.icePort = static_cast<std::uint16_t>(port_ == 65535 ? port_ - 1 : port_ + 1);
    config.advertiseHost = options_.advertiseAddress;
    config.maxConnections = std::max(kMaxHandshakes, max_);
    const auto identity = wire::bedrock::loadOrCreateNetherNetIdentity(std::string{kNetherNetIdentityFile});
    if (identity.unreadable)
        logger.warn([] { return "The saved NetherNet identity is unreadable, so a new one is made."; });
    if (!identity.saved)
        logger.warn([] { return "Could not save the NetherNet identity: players will be asked to trust it again."; });
    config.identityKey = identity.key;
    nether_ = std::make_unique<wire::bedrock::NetherNetServer>(std::move(config));
    nether_->onStatus([this] {
        const std::lock_guard guard{netherStatusLock_};
        return netherStatus_;
    });
    nether_->onConnect(
        [this](const std::shared_ptr<wire::bedrock::NetherNetLink>& link) { adopt(*link, link->peer(), link); });
    nether_->onMessage(
        [this](const std::shared_ptr<wire::bedrock::NetherNetLink>& link, std::span<const std::uint8_t> payload) {
            if (const auto found = clients_.find(link->peer()); found != clients_.end() && found->second.link == link)
                found->second.connection->receive(payload);
        });
    nether_->onDisconnect([this](const std::shared_ptr<wire::bedrock::NetherNetLink>& link) {
        if (const auto found = clients_.find(link->peer()); found != clients_.end() && found->second.link == link)
            release(link->peer());
    });
    refreshNetherNetStatus();
    if (const auto started = nether_->start(); !started) {
        logger.warn([this, &started] {
            return "Could not start NetherNet: " + started.error() + ". It needs TCP port " + std::to_string(port_) +
                   " and a UDP port of its own (`nethernet-port`).";
        });
        return false;
    }
    return true;
}

void BedrockStatus::refreshNetherNetStatus() {
    wire::bedrock::NetherNetStatus snapshot;
    snapshot.name = cleanLine(motd_ ? motd_() : std::string{"Basalt Proxy"});
    snapshot.version = std::string{kGameVersion};
    snapshot.level = subMotd_;
    snapshot.protocol = static_cast<std::uint32_t>(wire::bedrock::kNativeBedrockProtocol);
    snapshot.players = static_cast<std::uint32_t>((online_ ? online_() : 0) + clients_.size());
    snapshot.maxPlayers = static_cast<std::uint32_t>(max_);
    const std::lock_guard guard{netherStatusLock_};
    netherStatus_ = std::move(snapshot);
}

void BedrockStatus::tell(std::string_view player, std::string_view miniMessage) {
    const std::lock_guard lock{orders_};
    if (pending_.size() < kMaxOrders)
        pending_.push_back({Order::Kind::kTell, std::string{player}, std::string{miniMessage}});
}

void BedrockStatus::kick(std::string_view player, std::string_view miniMessage) {
    const std::lock_guard lock{orders_};
    if (pending_.size() < kMaxOrders)
        pending_.push_back({Order::Kind::kKick, std::string{player}, std::string{miniMessage}});
}

std::size_t BedrockStatus::playingCount() {
    const std::lock_guard lock{orders_};
    return playing_.size();
}

bool BedrockStatus::connect(std::string_view player, std::string_view backend) {
    if (!lookup_ || !lookup_(backend))
        return false;
    const std::lock_guard lock{orders_};
    if (!playing_.contains(std::string{player}) || pending_.size() >= kMaxOrders)
        return false;
    pending_.push_back({Order::Kind::kConnect, std::string{player}, std::string{backend}});
    return true;
}

void BedrockStatus::stop() {
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
    clients_.clear();
    if (nether_)
        nether_->stop();
    nether_.reset();
    server_.reset();
}

} // namespace basalt::proxy::runtime::detail
