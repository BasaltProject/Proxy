#pragma once

#include "JavaLink.hpp"

#include <basalt/wire/bedrock/BedrockConnection.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace basalt::proxy::runtime::detail {

/// One Bedrock player standing in for a Java client: what the backend says is told to the player in Bedrock's terms and
/// what the player does is told to the backend in Java's.
class BedrockSession {
public:
    BedrockSession(wire::bedrock::BedrockConnection& connection, config::BackendConfig backend, std::string address);

    [[nodiscard]] bool begin();
    void pump();
    void fromBedrock(wire::bedrock::BedrockPacket packet, wire::io::ByteBuffer& body);

    [[nodiscard]] bool finished() const noexcept {
        return link_ == nullptr || link_->state() == JavaLink::State::kClosed;
    }

private:
    void fromJava(std::int32_t id, std::span<const std::uint8_t> body);
    void joined(std::span<const std::uint8_t> body);
    void blockChanged(std::span<const std::uint8_t> body);
    void sectionChanged(std::span<const std::uint8_t> body);
    void batchFinished();
    void spawn();
    void chunk(std::span<const std::uint8_t> body);
    void teleported(std::span<const std::uint8_t> body);
    void chat(std::span<const std::uint8_t> body);
    void sendBedrock(wire::bedrock::BedrockPacket packet, const wire::io::ByteBuffer& body);
    void sendJava(std::int32_t id, const wire::io::ByteBuffer& body);
    void leave(const std::string& reason);

    wire::bedrock::BedrockConnection& connection_;
    config::BackendConfig backend_;
    std::string address_;
    std::unique_ptr<JavaLink> link_;
    std::vector<std::string> biomeKeys_;
    std::int32_t sections_ = 24;
    std::int32_t minSection_ = -4;
    std::int32_t pendingTeleport_ = -1;
    bool started_ = false;
    bool spawned_ = false;
    bool teleported_ = false;
    std::size_t chunks_ = 0;
    float x_ = 0.0F;
    float y_ = 64.0F;
    float z_ = 0.0F;
    float yaw_ = 0.0F;
    float pitch_ = 0.0F;
};

} // namespace basalt::proxy::runtime::detail
