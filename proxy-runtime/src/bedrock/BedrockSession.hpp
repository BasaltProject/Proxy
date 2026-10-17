#pragma once

#include "JavaLink.hpp"

#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/bedrock/BedrockConnection.hpp>
#include <basalt/wire/protocol/play/InventoryPackets.hpp>
#include <basalt/wire/text/LangTable.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace basalt::proxy::runtime::detail {

struct NativeForwarding;

/// One Bedrock player standing in for a Java client: what the backend says is told to the player in Bedrock's terms and
/// what the player does is told to the backend in Java's.
class BedrockSession {
public:
    using Events = std::function<void(runtime::PlayerEvent)>;

    BedrockSession(wire::bedrock::BedrockConnection& connection, config::BackendConfig backend, std::string address,
                   Events events = {}, std::shared_ptr<const wire::text::LangTable> language = nullptr,
                   std::shared_ptr<const NativeForwarding> forwarding = nullptr);
    ~BedrockSession();
    BedrockSession(const BedrockSession&) = delete;
    BedrockSession& operator=(const BedrockSession&) = delete;

    [[nodiscard]] const std::string& name() const noexcept { return javaName_; }

    void tell(std::string_view miniMessage);
    void kick(std::string_view miniMessage);
    [[nodiscard]] bool begin();
    [[nodiscard]] bool switchTo(config::BackendConfig target);
    void pump();
    void fromBedrock(wire::bedrock::BedrockPacket packet, wire::io::ByteBuffer& body);

    [[nodiscard]] bool finished() const noexcept {
        return link_ == nullptr || link_->state() == JavaLink::State::kClosed;
    }

private:
    [[nodiscard]] bool openLink();
    void fromJava(std::int32_t id, std::span<const std::uint8_t> body);
    void joined(std::span<const std::uint8_t> body);
    void listed(std::span<const std::uint8_t> body);
    void unlisted(std::span<const std::uint8_t> body);
    void spawnEntity(std::span<const std::uint8_t> body);
    void entityData(std::span<const std::uint8_t> body);
    void syncEntity(std::span<const std::uint8_t> body);
    void teleportEntity(std::span<const std::uint8_t> body);
    void removeEntities(std::span<const std::uint8_t> body);
    void moveEntity(std::int32_t id, double x, double y, double z, float yaw, float pitch, bool onGround);
    [[nodiscard]] static std::uint64_t runtimeId(std::int32_t javaId);
    void interact(const wire::bedrock::PlayerAuthInput& input);
    [[nodiscard]] wire::bedrock::BedrockItemStack
    stackOf(const std::optional<wire::protocol::play::WireItemStack>& item) const;
    void healthChanged(std::span<const std::uint8_t> body);
    void experienceChanged(std::span<const std::uint8_t> body);
    void timeChanged(std::span<const std::uint8_t> body);
    void gameEvent(std::span<const std::uint8_t> body);
    void sendInventory();
    void inventoryContent(std::span<const std::uint8_t> body);
    void inventorySlot(std::span<const std::uint8_t> body);
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
    Events events_;
    std::shared_ptr<const wire::text::LangTable> language_;
    std::shared_ptr<const NativeForwarding> forwarding_;
    wire::auth::SessionProfile profile_;
    std::string javaName_;
    std::string javaUuid_;
    bool announced_ = false;
    std::unique_ptr<JavaLink> link_;
    std::vector<std::string> biomeKeys_;
    std::unordered_map<std::string, std::string> names_;
    std::unordered_set<std::int32_t> mobs_;
    std::unordered_map<std::int32_t, std::array<float, 3>> waitingItems_;
    std::unordered_map<std::int32_t, std::pair<std::uint64_t, std::uint64_t>> others_;
    std::int32_t sections_ = 24;
    std::int32_t minSection_ = -4;
    std::int32_t pendingTeleport_ = -1;
    bool started_ = false;
    std::array<std::optional<wire::protocol::play::WireItemStack>, 46> inventory_{};

    std::int32_t sequence_ = 0;
    std::int32_t heldSlot_ = 0;
    bool switching_ = false;
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
