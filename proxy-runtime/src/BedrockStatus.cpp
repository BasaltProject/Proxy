#include "BedrockStatus.hpp"

#include <basalt/wire/bedrock/BedrockPacketIds.hpp>
#include <basalt/wire/common/logging/Logger.hpp>

#include <chrono>

namespace basalt::proxy::runtime::detail {
namespace {

constexpr std::string_view kGameVersion = "1.26.50";
constexpr int kPollTimeoutMillis = 50;
constexpr std::size_t kMaxHandshakes = 64;
constexpr std::string_view kTurnedAway = "Bedrock players cannot join this network yet.";

std::uint64_t nowMillis() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

BedrockStatus::BedrockStatus(std::uint16_t port, std::string motd, std::function<std::size_t()> online, std::size_t max)
    : port_(port), motd_(std::move(motd)), online_(std::move(online)), max_(max) {}

BedrockStatus::~BedrockStatus() { stop(); }

std::string BedrockStatus::advertisement() const {
    std::string line = "MCPE;" + motd_ + ";";
    line += std::to_string(wire::bedrock::kNativeBedrockProtocol) + ";";
    line += std::string{kGameVersion} + ";";
    line += std::to_string(online_ ? online_() : 0) + ";";
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
        auto client = std::make_unique<wire::bedrock::BedrockConnection>(session, keys_, std::string{kGameVersion});
        wire::bedrock::BedrockConnection& connection = *client;
        connection.onLogin([&connection](const wire::bedrock::LoginRequest& login) {
            wire::common::Logger{"Bedrock"}.info([&login] {
                return login.name + " tried to join from Bedrock" + (login.authenticated ? "" : " (unauthenticated)") +
                       " and was turned away.";
            });
            connection.disconnect(kTurnedAway);
        });
        clients_.insert_or_assign(session.peer(), std::move(client));
    });
    server_->onMessage([this](wire::bedrock::RakSession& session, std::span<const std::uint8_t> payload) {
        if (const auto found = clients_.find(session.peer()); found != clients_.end())
            found->second->receive(payload);
    });
    server_->onDisconnect([this](wire::bedrock::RakSession& session) { clients_.erase(session.peer()); });
    running_.store(true);
    thread_ = std::jthread{[this] {
        while (running_.load())
            server_->poll(nowMillis(), kPollTimeoutMillis);
    }};
    return true;
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
