#pragma once

#include "bedrock/BedrockSession.hpp"

#include <basalt/proxy/runtime/Admission.hpp>
#include <basalt/wire/bedrock/BedrockConnection.hpp>
#include <basalt/wire/bedrock/BedrockKeys.hpp>
#include <basalt/wire/bedrock/RakServer.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace basalt::proxy::runtime::detail {

/// Answers Bedrock server-list pings and runs each Bedrock client through login and play on its own thread.
class BedrockStatus {
public:
    using Route = std::function<std::optional<config::BackendConfig>()>;
    using Lookup = std::function<std::optional<config::BackendConfig>(std::string_view name)>;

    BedrockStatus(std::uint16_t port, std::string motd, std::function<std::size_t()> online, std::size_t max,
                  Route route = {}, BedrockSession::Events events = {},
                  std::shared_ptr<const wire::text::LangTable> language = nullptr, Lookup lookup = {},
                  std::shared_ptr<const NativeForwarding> forwarding = nullptr, bool requireAuthenticated = false,
                  const config::AdmissionConfig& admission = {}, BedrockSession::Commands commands = {});
    ~BedrockStatus();
    BedrockStatus(const BedrockStatus&) = delete;
    BedrockStatus& operator=(const BedrockStatus&) = delete;

    [[nodiscard]] bool start();
    [[nodiscard]] std::size_t playingCount();
    void tell(std::string_view player, std::string_view miniMessage);
    void kick(std::string_view player, std::string_view miniMessage);
    /// True when a Bedrock player of that name is playing and the backend exists; the move runs on the Bedrock thread.
    [[nodiscard]] bool connect(std::string_view player, std::string_view backend);
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept { return server_ ? server_->port() : port_; }

private:
    [[nodiscard]] std::string advertisement() const;

    std::uint16_t port_;
    std::string motd_;
    std::function<std::size_t()> online_;
    std::size_t max_;
    Route route_;
    BedrockSession::Events events_;
    std::shared_ptr<const wire::text::LangTable> language_;
    std::shared_ptr<const wire::bedrock::EcKeyPair> keys_;
    std::unique_ptr<wire::bedrock::RakServer> server_;

    struct Client {
        std::unique_ptr<wire::bedrock::BedrockConnection> connection;
        std::unique_ptr<BedrockSession> session;
    };

    std::unordered_map<wire::io::UdpEndpoint, Client> clients_;

    struct Order {
        enum class Kind { kTell, kKick, kConnect } kind = Kind::kTell;
        std::string player;
        std::string text;
    };

    Lookup lookup_;
    std::shared_ptr<const NativeForwarding> forwarding_;
    bool requireAuthenticated_ = false;
    Admission admission_;
    BedrockSession::Commands commands_;
    std::unordered_set<std::string> playing_;
    std::mutex orders_;
    std::vector<Order> pending_;
    std::atomic<bool> running_{false};
    std::jthread thread_;
};

} // namespace basalt::proxy::runtime::detail
