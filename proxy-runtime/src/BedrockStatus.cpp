#include "BedrockStatus.hpp"

#include <basalt/wire/bedrock/BedrockPacketIds.hpp>
#include <basalt/wire/common/logging/Logger.hpp>

#include <algorithm>
#include <chrono>

namespace basalt::proxy::runtime::detail {
namespace {

constexpr std::string_view kGameVersion = "1.26.50";
constexpr int kPollTimeoutMillis = 5;
constexpr std::size_t kMaxHandshakes = 64;
constexpr std::size_t kMaxOrders = 1024;
constexpr std::string_view kTurnedAway = "Bedrock players cannot join this network yet.";

std::uint64_t nowMillis() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

BedrockStatus::BedrockStatus(std::uint16_t port, std::string motd, std::function<std::size_t()> online, std::size_t max,
                             Route route, BedrockSession::Events events,
                             std::shared_ptr<const wire::text::LangTable> language)
    : port_(port), motd_(std::move(motd)), online_(std::move(online)), max_(max), route_(std::move(route)),
      events_(std::move(events)), language_(std::move(language)) {}

BedrockStatus::~BedrockStatus() { stop(); }

std::string BedrockStatus::advertisement() const {
    std::string line = "MCPE;" + motd_ + ";";
    line += std::to_string(wire::bedrock::kNativeBedrockProtocol) + ";";
    line += std::string{kGameVersion} + ";";
    const auto playing = static_cast<std::size_t>(
        std::ranges::count_if(clients_, [](const auto& entry) { return entry.second.session != nullptr; }));
    line += std::to_string((online_ ? online_() : 0) + playing) + ";";
    line += std::to_string(max_) + ";";
    line += std::to_string(server_ ? server_->guid() : 0) + ";";
    line += "Basalt Proxy;Survival;1;";
    line += std::to_string(port()) + ";" + std::to_string(port()) + ";";
    return line;
}

bool BedrockStatus::start() {
    wire::bedrock::RakServerConfig config;
    config.port = port_;
    config.guid = nowMillis() ^ 0x6261736c61747870ULL;
    config.maxSessions = kMaxHandshakes;
    auto keys = wire::bedrock::EcKeyPair::generate();
    if (!keys)
        return false;
    keys_ = std::make_shared<const wire::bedrock::EcKeyPair>(std::move(*keys));
    server_ = std::make_unique<wire::bedrock::RakServer>(config);
    if (!server_->start()) {
        server_.reset();
        return false;
    }
    server_->onAdvertise([this] { return advertisement(); });
    server_->onConnect([this](wire::bedrock::RakSession& session) {
        auto connection = std::make_unique<wire::bedrock::BedrockConnection>(session, keys_, std::string{kGameVersion});
        wire::bedrock::BedrockConnection& raw = *connection;
        const wire::io::UdpEndpoint peer = session.peer();
        raw.onLogin([](const wire::bedrock::LoginRequest& login) {
            wire::common::Logger{"Bedrock"}.info([&login] {
                return login.name + " is joining from Bedrock" + (login.authenticated ? "" : " (unauthenticated)") +
                       ".";
            });
        });
        raw.onReady([this, &raw, peer] {
            const auto found = clients_.find(peer);
            const auto backend = route_ ? route_() : std::nullopt;
            if (found == clients_.end() || !backend) {
                wire::common::Logger{"Bedrock"}.warn(
                    [] { return std::string{"No backend to send a Bedrock player to."}; });
                raw.disconnect(kTurnedAway);
                return;
            }
            found->second.session =
                std::make_unique<BedrockSession>(raw, *backend, peer.toString(), events_, language_);
            if (!found->second.session->begin()) {
                wire::common::Logger{"Bedrock"}.warn([] { return std::string{"The backend could not be resolved."}; });
                found->second.session.reset();
                raw.disconnect("The server could not be reached.");
            }
        });
        raw.onPacket([this, peer](wire::bedrock::BedrockPacket packet, wire::io::ByteBuffer& body) {
            if (const auto found = clients_.find(peer); found != clients_.end() && found->second.session)
                found->second.session->fromBedrock(packet, body);
        });
        clients_.insert_or_assign(peer, Client{std::move(connection), nullptr});
    });
    server_->onMessage([this](wire::bedrock::RakSession& session, std::span<const std::uint8_t> payload) {
        if (const auto found = clients_.find(session.peer()); found != clients_.end())
            found->second.connection->receive(payload);
    });
    server_->onDisconnect([this](wire::bedrock::RakSession& session) { clients_.erase(session.peer()); });
    running_.store(true);
    thread_ = std::jthread{[this] {
        while (running_.load()) {
            server_->poll(nowMillis(), kPollTimeoutMillis);
            std::vector<Order> orders;
            {
                const std::lock_guard lock{orders_};
                orders.swap(pending_);
            }
            for (const Order& order : orders) {
                for (auto& [peer, client] : clients_) {
                    if (!client.session || client.session->name() != order.player)
                        continue;
                    if (order.kick)
                        client.session->kick(order.text);
                    else
                        client.session->tell(order.text);
                }
            }
            for (auto& [peer, client] : clients_) {
                if (client.session)
                    client.session->pump();
            }
        }
    }};
    return true;
}

void BedrockStatus::tell(std::string_view player, std::string_view miniMessage) {
    const std::lock_guard lock{orders_};
    if (pending_.size() < kMaxOrders)
        pending_.push_back({false, std::string{player}, std::string{miniMessage}});
}

void BedrockStatus::kick(std::string_view player, std::string_view miniMessage) {
    const std::lock_guard lock{orders_};
    if (pending_.size() < kMaxOrders)
        pending_.push_back({true, std::string{player}, std::string{miniMessage}});
}

void BedrockStatus::stop() {
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
    clients_.clear();
    server_.reset();
}

} // namespace basalt::proxy::runtime::detail
