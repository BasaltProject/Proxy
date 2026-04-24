#pragma once

#include <basalt/wire/bedrock/BedrockConnection.hpp>
#include <basalt/wire/bedrock/BedrockKeys.hpp>
#include <basalt/wire/bedrock/RakServer.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace basalt::proxy::runtime::detail {

/// Answers Bedrock server-list pings and takes a Bedrock client through login so it can be told, by name, why it is
/// turned away: nothing translates Bedrock play yet.
class BedrockStatus {
public:
    BedrockStatus(std::uint16_t port, std::string motd, std::function<std::size_t()> online, std::size_t max);
    ~BedrockStatus();
    BedrockStatus(const BedrockStatus&) = delete;
    BedrockStatus& operator=(const BedrockStatus&) = delete;

    [[nodiscard]] bool start();
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept { return server_ ? server_->port() : port_; }

private:
    [[nodiscard]] std::string advertisement() const;

    std::uint16_t port_;
    std::string motd_;
    std::function<std::size_t()> online_;
    std::size_t max_;
    std::shared_ptr<const wire::bedrock::EcKeyPair> keys_;
    std::unique_ptr<wire::bedrock::RakServer> server_;
    std::unordered_map<wire::io::UdpEndpoint, std::unique_ptr<wire::bedrock::BedrockConnection>> clients_;
    std::atomic<bool> running_{false};
    std::jthread thread_;
};

} // namespace basalt::proxy::runtime::detail
