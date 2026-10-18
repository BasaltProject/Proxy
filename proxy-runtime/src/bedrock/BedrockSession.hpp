#pragma once

#include "JavaLink.hpp"

#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/bedrock/BedrockConnection.hpp>
#include <basalt/wire/protocol/play/EntityData.hpp>
#include <basalt/wire/protocol/play/InventoryPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/text/Component.hpp>
#include <basalt/wire/text/LangTable.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace basalt::proxy::runtime::detail {

struct NativeForwarding;

/// One Bedrock player standing in for a Java client: the backend is told in Java's terms, the player in Bedrock's.
class BedrockSession {
public:
    using Events = std::function<void(runtime::PlayerEvent)>;

    struct Commands {
        std::function<bool(std::string_view player, std::string_view command)> run;
        std::function<std::vector<runtime::PlayerCommandSpec>()> specs;
    };

    BedrockSession(wire::bedrock::BedrockConnection& connection, config::BackendConfig backend, std::string address,
                   std::uint16_t port, Events events = {},
                   std::shared_ptr<const wire::text::LangTable> language = nullptr,
                   std::shared_ptr<const NativeForwarding> forwarding = nullptr, Commands commands = {});
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
    [[nodiscard]] static std::uint64_t sharedFlags(std::uint8_t java);
    void nameTagged(const wire::protocol::play::SetEntityDataPacket& packet);
    void syncEntity(std::span<const std::uint8_t> body);
    void teleportEntity(std::span<const std::uint8_t> body);
    void removeEntities(std::span<const std::uint8_t> body);
    void moveEntity(std::int32_t id, double x, double y, double z, float yaw, float pitch, bool onGround);
    [[nodiscard]] static std::uint64_t runtimeId(std::int32_t javaId);
    void sprinting(bool now);
    void entityTransaction(wire::io::ByteBuffer& body);
    void interact(const wire::bedrock::PlayerAuthInput& input);
    [[nodiscard]] wire::bedrock::BedrockItemStack
    stackOf(const std::optional<wire::protocol::play::WireItemStack>& item) const;
    [[nodiscard]] static std::int32_t dimensionOf(std::string_view worldKey);
    void useDimension(std::int32_t dimension);
    void changeDimension();
    void respawned(std::span<const std::uint8_t> body);
    void died(std::span<const std::uint8_t> body);
    void playerChat(std::span<const std::uint8_t> body);
    void title(wire::bedrock::TitleType type, std::span<const std::uint8_t> body);
    void titleTimes(std::span<const std::uint8_t> body);
    void titlesCleared(std::span<const std::uint8_t> body);
    [[nodiscard]] std::string sidebarTitle() const;
    void drawSidebar();
    void objective(std::span<const std::uint8_t> body);
    void displayObjective(std::span<const std::uint8_t> body);
    void scoreSet(std::span<const std::uint8_t> body);
    void scoreReset(std::span<const std::uint8_t> body);
    void bossEvent(std::span<const std::uint8_t> body);
    [[nodiscard]] wire::text::Component resolved(const wire::text::Component& component) const;
    void healthChanged(std::span<const std::uint8_t> body);
    void experienceChanged(std::span<const std::uint8_t> body);
    void timeChanged(std::span<const std::uint8_t> body);
    void applyGamemode(std::int32_t mode);
    void gameEvent(std::span<const std::uint8_t> body);
    void sendInventory();
    [[nodiscard]] static std::optional<wire::bedrock::BedrockContainerType> windowType(std::int32_t menu,
                                                                                       std::size_t& slots);
    void screenOpened(std::span<const std::uint8_t> body);
    void screenClosed(std::span<const std::uint8_t> body);
    void windowContent(const wire::protocol::play::ContainerSetContentPacket& packet);
    void drawWindow();
    void closeWindow(bool tellBackend);
    void stackRequests(wire::io::ByteBuffer& body);
    void stackRequest(const wire::bedrock::BedrockStackRequest& request);
    [[nodiscard]] bool clickFor(const wire::bedrock::BedrockStackAction& action);
    [[nodiscard]] std::optional<std::int32_t> clickSlot(const wire::bedrock::BedrockStackSlot& slot) const;
    [[nodiscard]] const std::optional<wire::protocol::play::WireItemStack>& itemAt(std::int32_t javaSlot) const;
    void click(std::int32_t javaSlot, std::int8_t button,
               wire::protocol::play::ServerboundContainerClickPacket::Mode mode);
    void rejectStackRequest(std::int32_t requestId);
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
    void tickEnded();
    void commandList(std::span<const std::uint8_t> body);
    void kicked(std::span<const std::uint8_t> body);
    void leave(const std::string& reason);

    wire::bedrock::BedrockConnection& connection_;
    config::BackendConfig backend_;
    std::string address_;
    Commands commands_;
    std::uint16_t port_ = 0;
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

    struct NameTag {
        std::string text;
        bool always = false;
        std::uint64_t flags = wire::bedrock::flagBit(wire::bedrock::EntityFlag::HasCollision) |
                              wire::bedrock::flagBit(wire::bedrock::EntityFlag::HasGravity) |
                              wire::bedrock::flagBit(wire::bedrock::EntityFlag::Breathing) |
                              wire::bedrock::flagBit(wire::bedrock::EntityFlag::CanShowName);
    };

    std::unordered_map<std::int32_t, NameTag> nameTags_;
    std::unordered_map<std::string, std::int64_t> bossBars_;

    struct ScoreLine {
        std::int64_t id = 0;
        std::string text;
        std::int32_t score = 0;
    };

    struct Objective {
        std::string title;
        std::map<std::string, ScoreLine> lines;
    };

    std::unordered_map<std::string, Objective> objectives_;
    std::string sidebar_;
    std::int64_t nextLine_ = 0;
    std::unordered_map<std::int32_t, std::array<float, 3>> waitingItems_;
    std::unordered_map<std::int32_t, std::pair<std::uint64_t, std::uint64_t>> others_;
    std::int32_t sections_ = 24;
    std::int32_t minSection_ = -4;
    std::int32_t pendingTeleport_ = -1;
    bool started_ = false;
    std::array<std::optional<wire::protocol::play::WireItemStack>, 46> inventory_{};

    struct Window {
        std::int32_t id = 0;
        wire::bedrock::BedrockContainerType type = wire::bedrock::BedrockContainerType::Container;
        std::size_t slots = 0;
        std::vector<std::optional<wire::protocol::play::WireItemStack>> items;
        bool shown = false;
    };

    std::optional<Window> window_;
    std::int32_t stateId_ = 0;
    std::array<std::int32_t, 3> lastUse_{};

    std::chrono::steady_clock::time_point joinStarted_{};
    std::chrono::steady_clock::time_point lastFromJava_{};
    std::int32_t sequence_ = 0;
    std::int32_t heldSlot_ = 0;
    bool switching_ = false;
    bool respawning_ = false;
    bool sprinting_ = false;
    std::int32_t selfEntity_ = 0;
    std::int32_t dimension_ = 0;
    std::int32_t clientDimension_ = 0;
    std::int32_t gamemode_ = 0;
    bool spawned_ = false;
    bool teleported_ = false;
    bool arriving_ = false;
    std::int32_t acceptedTeleport_ = -1;
    std::chrono::steady_clock::time_point holdPositionsUntil_{};
    std::chrono::steady_clock::time_point lastPosition_{};
    std::optional<wire::protocol::play::ServerboundPositionLookPacket> pendingMove_;
    int traced_ = 0;
    std::int32_t arrivalInputs_ = 0;
    std::size_t chunks_ = 0;
    float x_ = 0.0F;
    float y_ = 64.0F;
    float z_ = 0.0F;
    float yaw_ = 0.0F;
    float pitch_ = 0.0F;
};

} // namespace basalt::proxy::runtime::detail
