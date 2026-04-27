#include "BedrockSession.hpp"

#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/bedrock/BedrockBiomes.hpp>
#include <basalt/wire/bedrock/BedrockEntityIdentifiers.hpp>
#include <basalt/wire/bedrock/BedrockItems.hpp>
#include <basalt/wire/bedrock/BedrockPackets.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/play/CommandPackets.hpp>
#include <basalt/wire/protocol/play/EntityData.hpp>
#include <basalt/wire/protocol/play/EntityPackets.hpp>
#include <basalt/wire/protocol/play/EntityTypeIds.hpp>
#include <basalt/wire/protocol/play/InventoryPackets.hpp>
#include <basalt/wire/protocol/play/ItemIds.hpp>
#include <basalt/wire/protocol/play/PalettedContainer.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/play/PlayerStatePackets.hpp>
#include <basalt/wire/protocol/play/WorldPackets.hpp>
#include <basalt/wire/text/MiniMessage.hpp>
#include <basalt/wire/text/Serializers.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <variant>

namespace basalt::proxy::runtime::detail {
namespace {

namespace bedrock = wire::bedrock;
namespace play = wire::protocol::play;
using bedrock::BedrockPacket;

constexpr std::int32_t kInitialRadius = 4;
constexpr float kChunksPerTick = 9.0F;
constexpr std::uint64_t kOthersBase = 1000;
constexpr float kMobWidth = 0.6F;
constexpr float kMobHeight = 1.8F;
constexpr std::size_t kCarriedSlots = 36;
constexpr std::size_t kJavaPlayerSlots = 46;

// Bedrock numbers the hotbar first; Java puts it last, after the 27 of the main inventory.
std::size_t javaSlot(std::size_t bedrockSlot) { return bedrockSlot < 9 ? 36 + bedrockSlot : bedrockSlot; }

constexpr std::size_t kBlocksPerSection = 4096;
constexpr std::size_t kBiomesPerSection = 64;
constexpr std::chrono::milliseconds kConnectBudget{5000};
constexpr std::int32_t kChatMessageId = play::ChatMessagePacket::kId;
constexpr std::int32_t kChatCommandId = play::ChatCommandPacket::kId;

[[nodiscard]] std::string javaName(const std::string& gamertag) {
    std::string name = "_";
    for (const char c : gamertag) {
        if (name.size() == 16)
            break;
        name += (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') ? c : '_';
    }
    return name;
}

} // namespace

BedrockSession::BedrockSession(bedrock::BedrockConnection& connection, config::BackendConfig backend,
                               std::string address, Events events)
    : connection_(connection), backend_(std::move(backend)), address_(std::move(address)), events_(std::move(events)) {}

BedrockSession::~BedrockSession() {
    if (announced_ && events_)
        events_({runtime::PlayerEventKind::kLeave, javaName_, backend_.name, javaUuid_, {}});
}

bool BedrockSession::begin() {
    const bedrock::LoginRequest& login = connection_.login();
    wire::auth::SessionProfile profile;
    profile.name = javaName(login.name);
    profile.id = wire::common::Uuid::offlinePlayer(profile.name);
    javaName_ = profile.name;
    javaUuid_ = profile.id.toString();
    JavaLink::Events events;
    events.play = [this](std::int32_t id, std::span<const std::uint8_t> body) { fromJava(id, body); };
    events.registry = [this](const std::string& registry, std::vector<std::string> keys) {
        if (registry == "minecraft:worldgen/biome")
            biomeKeys_ = std::move(keys);
    };
    events.closed = [this](const std::string& reason) { leave(reason); };
    link_ = std::make_unique<JavaLink>(backend_, std::move(profile), address_, std::move(events));
    return link_->begin(kConnectBudget);
}

void BedrockSession::tell(std::string_view miniMessage) {
    wire::io::ByteBuffer out;
    bedrock::writeText(out, bedrock::TextType::Raw, "",
                       wire::text::toLegacyText(wire::text::miniMessage::parse(miniMessage)), "");
    sendBedrock(BedrockPacket::Text, out);
    connection_.flush();
}

void BedrockSession::kick(std::string_view miniMessage) {
    leave(wire::text::toPlainText(wire::text::miniMessage::parse(miniMessage)));
}

void BedrockSession::pump() {
    if (link_ != nullptr)
        link_->pump();
}

void BedrockSession::leave(const std::string& reason) {
    wire::common::Logger{"Bedrock"}.info([&] { return connection_.login().name + " left: " + reason; });
    if (connection_.state() != bedrock::BedrockState::Closed)
        connection_.disconnect(reason);
}

void BedrockSession::sendBedrock(BedrockPacket packet, const wire::io::ByteBuffer& body) {
    connection_.sendPacket(packet, body.readable());
}

void BedrockSession::sendJava(std::int32_t id, const wire::io::ByteBuffer& body) {
    if (link_ != nullptr)
        link_->send(id, body.readable());
}

void BedrockSession::fromJava(std::int32_t id, std::span<const std::uint8_t> body) {
    if (id == play::LoginPlayPacket::kId)
        joined(body);
    else if (id == play::MapChunkPacket::kId)
        chunk(body);
    else if (id == play::PlayerPositionPacket::kId)
        teleported(body);
    else if (id == play::SystemChatPacket::kId)
        chat(body);
    else if (id == play::PlayerInfoUpdatePacket::kId)
        listed(body);
    else if (id == play::PlayerInfoRemovePacket::kId)
        unlisted(body);
    else if (id == play::SpawnEntityPacket::kId)
        spawnEntity(body);
    else if (id == play::SetEntityDataPacket::kId)
        entityData(body);
    else if (id == play::EntityPositionSyncPacket::kId)
        syncEntity(body);
    else if (id == play::EntityTeleportPacket::kId)
        teleportEntity(body);
    else if (id == play::RemoveEntitiesPacket::kId)
        removeEntities(body);
    else if (id == play::ContainerSetContentPacket::kId)
        inventoryContent(body);
    else if (id == play::ContainerSetSlotPacket::kId)
        inventorySlot(body);
    else if (id == play::ClientboundBlockUpdatePacket::kId)
        blockChanged(body);
    else if (id == play::SectionBlocksUpdatePacket::kId)
        sectionChanged(body);
    else if (id == play::ChunkBatchFinishedPacket::kId)
        batchFinished();
    else if (id == play::PlayDisconnectPacket::kId)
        leave("The server closed the connection.");
    connection_.flush();
}

void BedrockSession::joined(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto login = play::LoginPlayPacket::read(in);
    if (!login || started_)
        return;
    started_ = true;
    const int protocol = connection_.protocolVersion();
    sections_ = 24;
    minSection_ = -4;

    bedrock::StartGameInfo info;
    info.x = x_;
    info.y = y_ + bedrock::kPlayerEyeHeight;
    info.z = z_;
    info.playerGameType = static_cast<std::int32_t>(login->spawnInfo.gamemode);
    info.vanillaVersion = "1.26.50";
    info.serverId = "basalt-proxy";
    info.worldId = "basalt-proxy";
    wire::io::ByteBuffer out;
    bedrock::writeStartGame(out, info, protocol);
    sendBedrock(BedrockPacket::StartGame, out);
    out.clear();
    bedrock::writeBiomeDefinitionList(out);
    sendBedrock(BedrockPacket::BiomeDefinitionList, out);
    out.clear();
    bedrock::writeItemComponent(out, protocol);
    sendBedrock(BedrockPacket::ItemComponent, out);
    out.clear();
    bedrock::writeAvailableEntityIdentifiers(out);
    sendBedrock(BedrockPacket::AvailableEntityIdentifiers, out);
    out.clear();
    bedrock::writeCreativeContent(out, protocol);
    sendBedrock(BedrockPacket::CreativeContent, out);
    out.clear();
    bedrock::writeGameRulesChanged(out, protocol);
    sendBedrock(BedrockPacket::GameRulesChanged, out);
    out.clear();
    bedrock::writeCameraPresets(out, protocol);
    sendBedrock(BedrockPacket::CameraPresets, out);
    out.clear();
    bedrock::writeCraftingData(out, {}, protocol);
    sendBedrock(BedrockPacket::CraftingData, out);
    out.clear();
    bedrock::writeSetCommandsEnabled(out, true);
    sendBedrock(BedrockPacket::SetCommandsEnabled, out);
    out.clear();
    bedrock::writeChunkRadiusUpdated(out, kInitialRadius);
    sendBedrock(BedrockPacket::ChunkRadiusUpdated, out);
    out.clear();
    bedrock::writeNetworkChunkPublisherUpdate(out, static_cast<std::int32_t>(x_), static_cast<std::int32_t>(y_),
                                              static_cast<std::int32_t>(z_), kInitialRadius * 16);
    sendBedrock(BedrockPacket::NetworkChunkPublisherUpdate, out);
}

std::uint64_t BedrockSession::runtimeId(std::int32_t javaId) {
    return static_cast<std::uint64_t>(javaId) + kOthersBase;
}

void BedrockSession::listed(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::PlayerInfoUpdatePacket::read(in);
    if (!packet || (packet->actions & play::PlayerInfoUpdatePacket::kAddPlayer) == 0)
        return;
    for (const auto& entry : packet->entries)
        names_[entry.uuid.toString()] = entry.name;
}

void BedrockSession::unlisted(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::PlayerInfoRemovePacket::read(in);
    if (!packet)
        return;
    for (const auto& uuid : packet->players)
        names_.erase(uuid.toString());
}

void BedrockSession::spawnEntity(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SpawnEntityPacket::read(in);
    if (!packet || !spawned_)
        return;
    if (packet->entityType == play::SpawnEntityPacket::kItemEntityType) {
        waitingItems_[packet->entityId] = {static_cast<float>(packet->x), static_cast<float>(packet->y),
                                           static_cast<float>(packet->z)};
        return;
    }
    if (packet->entityType != play::SpawnEntityPacket::kPlayerEntityType) {
        const std::string_view path = play::javaEntityTypeName(packet->entityType);
        const std::string_view name = bedrock::bedrockEntityName("minecraft:" + std::string{path});
        if (path.empty() || name.empty())
            return;
        wire::io::ByteBuffer out;
        bedrock::writeAddEntity(out, static_cast<std::int64_t>(runtimeId(packet->entityId)), name,
                                static_cast<float>(packet->x), static_cast<float>(packet->y),
                                static_cast<float>(packet->z), packet->yaw, packet->pitch, kMobWidth, kMobHeight, 0,
                                connection_.protocolVersion());
        sendBedrock(BedrockPacket::AddEntity, out);
        mobs_.insert(packet->entityId);
        return;
    }
    const auto known = names_.find(packet->uuid.toString());
    const std::string name = known != names_.end() ? known->second : std::string{"Player"};
    const int protocol = connection_.protocolVersion();
    bedrock::PlayerListEntry entry;
    entry.uuidHigh = packet->uuid.high();
    entry.uuidLow = packet->uuid.low();
    entry.entityId = static_cast<std::int64_t>(runtimeId(packet->entityId));
    entry.name = name;
    entry.skin = bedrock::flatSkin(name);
    wire::io::ByteBuffer out;
    bedrock::writePlayerListAdd(out, std::span{&entry, 1}, protocol);
    sendBedrock(BedrockPacket::PlayerList, out);
    bedrock::AddPlayerInfo add;
    add.uuidHigh = entry.uuidHigh;
    add.uuidLow = entry.uuidLow;
    add.entityId = entry.entityId;
    add.runtimeEntityId = runtimeId(packet->entityId);
    add.name = name;
    add.x = static_cast<float>(packet->x);
    add.y = static_cast<float>(packet->y) + bedrock::kPlayerEyeHeight;
    add.z = static_cast<float>(packet->z);
    add.yaw = packet->yaw;
    add.pitch = packet->pitch;
    out.clear();
    bedrock::writeAddPlayer(out, add, protocol);
    sendBedrock(BedrockPacket::AddPlayer, out);
    others_[packet->entityId] = {entry.uuidHigh, entry.uuidLow};
}

void BedrockSession::moveEntity(std::int32_t id, double x, double y, double z, float yaw, float pitch, bool onGround) {
    const bool mob = mobs_.contains(id);
    if (!mob && !others_.contains(id))
        return;
    wire::io::ByteBuffer out;
    bedrock::writeMoveEntity(out, runtimeId(id), static_cast<float>(x),
                             static_cast<float>(y) + (mob ? 0.0F : bedrock::kPlayerEyeHeight), static_cast<float>(z),
                             yaw, pitch, onGround);
    sendBedrock(BedrockPacket::MoveEntityAbsolute, out);
}

void BedrockSession::entityData(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetEntityDataPacket::read(in);
    if (!packet)
        return;
    const auto waiting = waitingItems_.find(packet->entityId);
    if (waiting == waitingItems_.end())
        return;
    for (const auto& entry : packet->entries) {
        const auto* item = std::get_if<play::SetEntityDataPacket::ItemValue>(&entry.value);
        if (entry.index != play::SetEntityDataPacket::kItemStackIndex || item == nullptr)
            continue;
        const std::array<float, 3> at = waiting->second;
        waitingItems_.erase(waiting);
        wire::io::ByteBuffer out;
        bedrock::writeAddItemEntity(out, static_cast<std::int64_t>(runtimeId(packet->entityId)), stackOf(item->stack),
                                    at[0], at[1], at[2], 0.0F, 0.0F, 0.0F, connection_.protocolVersion());
        sendBedrock(BedrockPacket::AddItemEntity, out);
        mobs_.insert(packet->entityId);
        return;
    }
}

void BedrockSession::syncEntity(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    if (const auto packet = play::EntityPositionSyncPacket::read(in))
        moveEntity(packet->entityId, packet->x, packet->y, packet->z, packet->yaw, packet->pitch, packet->onGround);
}

void BedrockSession::teleportEntity(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    if (const auto packet = play::EntityTeleportPacket::read(in))
        moveEntity(packet->entityId, packet->x, packet->y, packet->z, packet->yaw, packet->pitch, packet->onGround);
}

void BedrockSession::removeEntities(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::RemoveEntitiesPacket::read(in);
    if (!packet)
        return;
    for (const std::int32_t id : packet->entityIds) {
        waitingItems_.erase(id);
        if (mobs_.erase(id) != 0) {
            wire::io::ByteBuffer gone;
            bedrock::writeRemoveEntity(gone, static_cast<std::int64_t>(runtimeId(id)));
            sendBedrock(BedrockPacket::RemoveEntity, gone);
            continue;
        }
        const auto found = others_.find(id);
        if (found == others_.end())
            continue;
        wire::io::ByteBuffer out;
        bedrock::writeRemoveEntity(out, static_cast<std::int64_t>(runtimeId(id)));
        sendBedrock(BedrockPacket::RemoveEntity, out);
        const std::pair<std::uint64_t, std::uint64_t> uuid = found->second;
        out.clear();
        bedrock::writePlayerListRemove(out, std::span{&uuid, 1}, connection_.protocolVersion());
        sendBedrock(BedrockPacket::PlayerList, out);
        others_.erase(found);
    }
}

bedrock::BedrockItemStack BedrockSession::stackOf(const std::optional<play::WireItemStack>& item) const {
    if (!item || item->count <= 0)
        return {};
    const std::string_view path = play::javaItemName(item->itemId);
    if (path.empty())
        return {};
    const bedrock::ItemName* named = bedrock::bedrockItem("minecraft:" + std::string{path});
    if (named == nullptr)
        return {};
    return {.runtimeId = named->palettePlace + 1, .count = item->count, .data = named->data, .blockRuntimeId = 0};
}

void BedrockSession::sendInventory() {
    std::array<bedrock::BedrockItemStack, kCarriedSlots> carried{};
    for (std::size_t slot = 0; slot < kCarriedSlots; ++slot)
        carried[slot] = stackOf(inventory_[javaSlot(slot)]);
    wire::io::ByteBuffer out;
    bedrock::writeInventoryContent(out, 0, carried, bedrock::BedrockContainerSlot::HotbarAndInventory,
                                   connection_.protocolVersion());
    sendBedrock(BedrockPacket::InventoryContent, out);
}

void BedrockSession::inventoryContent(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::ContainerSetContentPacket::read(in);
    if (!packet || packet->containerId != play::ContainerSetContentPacket::kPlayerInventory ||
        packet->items.size() < kJavaPlayerSlots)
        return;
    for (std::size_t slot = 0; slot < kJavaPlayerSlots; ++slot)
        inventory_[slot] = packet->items[slot];
    sendInventory();
}

void BedrockSession::inventorySlot(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::ContainerSetSlotPacket::read(in);
    if (!packet || packet->containerId != play::ContainerSetContentPacket::kPlayerInventory || packet->slot < 0 ||
        static_cast<std::size_t>(packet->slot) >= kJavaPlayerSlots)
        return;
    inventory_[static_cast<std::size_t>(packet->slot)] = packet->item;
    sendInventory();
}

void BedrockSession::blockChanged(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::ClientboundBlockUpdatePacket::read(in);
    if (!packet)
        return;
    wire::io::ByteBuffer out;
    bedrock::writeUpdateBlock(out, packet->pos.x, packet->pos.y, packet->pos.z, packet->blockStateId,
                              connection_.protocolVersion());
    sendBedrock(BedrockPacket::UpdateBlock, out);
}

void BedrockSession::sectionChanged(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SectionBlocksUpdatePacket::read(in);
    if (!packet)
        return;
    for (const auto& change : packet->changes) {
        const auto at = static_cast<std::int32_t>(change.position);
        wire::io::ByteBuffer out;
        bedrock::writeUpdateBlock(out, packet->sectionX * 16 + ((at >> 8) & 15), packet->sectionY * 16 + (at & 15),
                                  packet->sectionZ * 16 + ((at >> 4) & 15), change.blockStateId,
                                  connection_.protocolVersion());
        sendBedrock(BedrockPacket::UpdateBlock, out);
    }
}

void BedrockSession::batchFinished() {
    wire::io::ByteBuffer out;
    play::ChunkBatchReceivedPacket{kChunksPerTick}.write(out);
    sendJava(play::ChunkBatchReceivedPacket::kId, out);
    spawn();
}

void BedrockSession::chunk(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::MapChunkPacket::read(in);
    if (!packet) {
        wire::common::Logger{"Bedrock"}.warn([] { return std::string{"A chunk from the backend could not be read."}; });
        return;
    }
    const int protocol = connection_.protocolVersion();
    wire::io::ByteBuffer data;
    data.writeBytes(packet->chunkData);
    wire::io::ByteBuffer packed;
    wire::io::ByteBuffer biomes;
    std::int32_t sent = 0;
    std::vector<std::array<std::int32_t, kBlocksPerSection>> columns;
    for (std::int32_t section = 0; section < sections_ && data.readableBytes() != 0; ++section) {
        static_cast<void>(data.readShort());
        static_cast<void>(data.readShort());
        std::vector<std::int32_t> blocks =
            play::palettedContainer::read(data, kBlocksPerSection, play::palettedContainer::kBlockMaxIndirectBits);
        const std::vector<std::int32_t> quarts =
            play::palettedContainer::read(data, kBiomesPerSection, play::palettedContainer::kBiomeMaxIndirectBits);
        if (data.failed() || blocks.size() != kBlocksPerSection || quarts.size() != kBiomesPerSection)
            return;
        bool solid = false;
        for (const std::int32_t state : blocks)
            solid = solid || state != 0;
        if (solid)
            sent = section + 1;
        std::array<std::int32_t, kBlocksPerSection> ordered{};
        std::copy(blocks.begin(), blocks.end(), ordered.begin());
        columns.push_back(ordered);
        bedrock::BiomeQuarts mapped{};
        for (std::size_t quart = 0; quart < kBiomesPerSection; ++quart) {
            const auto id = static_cast<std::size_t>(quarts[quart]);
            const auto found = id < biomeKeys_.size() ? bedrock::bedrockBiomeId(biomeKeys_[id]) : std::nullopt;
            mapped[quart] = found.value_or(bedrock::kFallbackBedrockBiome);
        }
        bedrock::writeBiomeSection(biomes, mapped);
    }
    for (std::int32_t section = 0; section < sent; ++section)
        bedrock::writeSubChunk(packed, columns[static_cast<std::size_t>(section)],
                               static_cast<std::int8_t>(minSection_ + section), protocol);
    wire::io::ByteBuffer out;
    bedrock::writeLevelChunk(out, packet->x, packet->z, static_cast<std::uint32_t>(sent), packed.readable(),
                             static_cast<std::uint32_t>(columns.size()), 0, protocol, {}, biomes.readable());
    sendBedrock(BedrockPacket::LevelChunk, out);
    chunks_++;
}

void BedrockSession::teleported(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto position = play::PlayerPositionPacket::read(in);
    if (!position)
        return;
    x_ = static_cast<float>(position->x);
    y_ = static_cast<float>(position->y);
    z_ = static_cast<float>(position->z);
    yaw_ = position->yaw;
    pitch_ = position->pitch;
    wire::io::ByteBuffer out;
    bedrock::writeTeleport(out, 1, x_, y_ + bedrock::kPlayerEyeHeight, z_, yaw_, pitch_, 0,
                           connection_.protocolVersion());
    sendBedrock(BedrockPacket::MovePlayer, out);
    out.clear();
    play::AcceptTeleportationPacket{.teleportId = position->teleportId,
                                    .x = position->x,
                                    .y = position->y,
                                    .z = position->z,
                                    .yaw = yaw_,
                                    .pitch = pitch_}
        .write(out);
    sendJava(play::AcceptTeleportationPacket::kId, out);
    teleported_ = true;
    out.clear();
    sendJava(play::kPlayerLoadedId, out);
    spawn();
}

void BedrockSession::spawn() {
    if (spawned_ || !teleported_ || chunks_ == 0)
        return;
    spawned_ = true;
    if (events_ && !announced_) {
        announced_ = true;
        events_({runtime::PlayerEventKind::kJoin, javaName_, backend_.name, javaUuid_, {}});
    }
    wire::io::ByteBuffer out;
    bedrock::writePlayStatus(out, bedrock::PlayStatus::PlayerSpawn);
    sendBedrock(BedrockPacket::PlayStatus, out);
}

void BedrockSession::chat(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SystemChatPacket::read(in);
    if (!packet)
        return;
    wire::io::ByteBuffer out;
    bedrock::writeText(out, bedrock::TextType::Raw, "", wire::text::toLegacyText(packet->content), "");
    sendBedrock(BedrockPacket::Text, out);
}

void BedrockSession::interact(const bedrock::PlayerAuthInput& input) {
    const auto direction = [](std::int32_t face) { return static_cast<play::Direction>(std::clamp(face, 0, 5)); };
    for (const bedrock::BedrockBlockAction& action : input.blockActions) {
        play::ServerboundPlayerActionPacket packet;
        packet.pos = {action.x, action.y, action.z};
        packet.direction = direction(action.face);
        packet.sequence = ++sequence_;
        if (action.action == static_cast<std::int32_t>(bedrock::BedrockBlockActionType::StartBreak))
            packet.action = play::ServerboundPlayerActionPacket::Action::StartDestroyBlock;
        else if (action.action == static_cast<std::int32_t>(bedrock::BedrockBlockActionType::AbortBreak))
            packet.action = play::ServerboundPlayerActionPacket::Action::AbortDestroyBlock;
        else if (action.action == static_cast<std::int32_t>(bedrock::BedrockBlockActionType::StopBreak) ||
                 action.action == static_cast<std::int32_t>(bedrock::BedrockBlockActionType::PredictDestroy))
            packet.action = play::ServerboundPlayerActionPacket::Action::StopDestroyBlock;
        else
            continue;
        wire::io::ByteBuffer out;
        packet.write(out);
        sendJava(play::ServerboundPlayerActionPacket::kId, out);
    }
    if (!input.itemUse || input.itemUse->action != 0)
        return;
    const bedrock::BedrockItemUse& use = *input.itemUse;
    wire::io::ByteBuffer out;
    if (use.hotbarSlot != heldSlot_ && use.hotbarSlot >= 0 && use.hotbarSlot < 9) {
        heldSlot_ = use.hotbarSlot;
        play::ServerboundSetCarriedItemPacket{static_cast<std::int16_t>(heldSlot_)}.write(out);
        sendJava(play::ServerboundSetCarriedItemPacket::kId, out);
        out.clear();
    }
    play::ServerboundUseItemOnPacket packet;
    packet.blockHit.pos = {use.x, use.y, use.z};
    packet.blockHit.direction = direction(use.face);
    packet.blockHit.clickX = use.clickX;
    packet.blockHit.clickY = use.clickY;
    packet.blockHit.clickZ = use.clickZ;
    packet.sequence = ++sequence_;
    packet.write(out);
    sendJava(play::ServerboundUseItemOnPacket::kId, out);
}

void BedrockSession::fromBedrock(BedrockPacket packet, wire::io::ByteBuffer& body) {
    if (packet == BedrockPacket::RequestChunkRadius) {
        const auto radius = bedrock::readRequestChunkRadius(body);
        if (!radius)
            return;
        wire::io::ByteBuffer out;
        bedrock::writeChunkRadiusUpdated(out, *radius);
        sendBedrock(BedrockPacket::ChunkRadiusUpdated, out);
        connection_.flush();
    } else if (packet == BedrockPacket::PlayerAuthInput) {
        const auto input = bedrock::readPlayerAuthInput(body, connection_.protocolVersion());
        if (!input || !teleported_)
            return;
        wire::io::ByteBuffer out;
        play::ServerboundPositionLookPacket move;
        move.x = static_cast<double>(input->x);
        move.y = static_cast<double>(input->y) - static_cast<double>(bedrock::kPlayerEyeHeight);
        move.z = static_cast<double>(input->z);
        move.yaw = input->yaw;
        move.pitch = input->pitch;
        move.onGround = input->onGround;
        move.write(out);
        sendJava(play::ServerboundPositionLookPacket::kId, out);
        interact(*input);
    } else if (packet == BedrockPacket::Text) {
        const auto text = bedrock::readText(body);
        if (!text)
            return;
        wire::io::ByteBuffer out;
        play::ChatMessagePacket message;
        if (events_)
            events_({runtime::PlayerEventKind::kChat, javaName_, {}, {}, text->message});
        message.message = text->message;
        message.timestamp =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        message.write(out);
        sendJava(kChatMessageId, out);
    } else if (packet == BedrockPacket::CommandRequest) {
        auto command = bedrock::readCommandRequest(body);
        if (!command)
            return;
        wire::io::ByteBuffer out;
        play::ChatCommandPacket typed;
        typed.command = command->starts_with('/') ? command->substr(1) : *command;
        typed.write(out);
        sendJava(kChatCommandId, out);
    }
}

} // namespace basalt::proxy::runtime::detail
