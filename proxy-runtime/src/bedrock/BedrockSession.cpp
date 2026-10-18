#include "BedrockSession.hpp"

#include "../NativeForwarding.hpp"

#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/bedrock/BedrockBiomes.hpp>
#include <basalt/wire/bedrock/BedrockColumn.hpp>
#include <basalt/wire/bedrock/BedrockEntityIdentifiers.hpp>
#include <basalt/wire/bedrock/BedrockItems.hpp>
#include <basalt/wire/bedrock/BedrockPackets.hpp>
#include <basalt/wire/bedrock/BedrockTypes.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/play/ChatPackets.hpp>
#include <basalt/wire/protocol/play/CommandPackets.hpp>
#include <basalt/wire/protocol/play/EntityData.hpp>
#include <basalt/wire/protocol/play/EntityPackets.hpp>
#include <basalt/wire/protocol/play/EntityTypeIds.hpp>
#include <basalt/wire/protocol/play/HudPackets.hpp>
#include <basalt/wire/protocol/play/InventoryPackets.hpp>
#include <basalt/wire/protocol/play/ItemIds.hpp>
#include <basalt/wire/protocol/play/PalettedContainer.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/play/PlayerStatePackets.hpp>
#include <basalt/wire/protocol/play/UiPackets.hpp>
#include <basalt/wire/protocol/play/WorldPackets.hpp>
#include <basalt/wire/text/MiniMessage.hpp>
#include <basalt/wire/text/Serializers.hpp>
#include <basalt/wire/text/TranslationResolver.hpp>

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
constexpr std::int64_t kTicksPerDay = 24000;
constexpr std::int32_t kOverworldMinSection = -4;
constexpr std::uint8_t kBabyIndex = 16;
constexpr std::uint64_t kSharedMask =
    bedrock::flagBit(bedrock::EntityFlag::OnFire) | bedrock::flagBit(bedrock::EntityFlag::Sneaking) |
    bedrock::flagBit(bedrock::EntityFlag::Sprinting) | bedrock::flagBit(bedrock::EntityFlag::Invisible) |
    bedrock::flagBit(bedrock::EntityFlag::Gliding);
constexpr std::int32_t kSwingAction = 1;
constexpr std::int32_t kAttackAction = 1;
constexpr std::int32_t kTitleKeep = -1;
constexpr std::int64_t kBossBase = 0x40000000;
constexpr float kMaxHealth = 20.0F;
constexpr float kMaxFood = 20.0F;
constexpr float kMaxLevel = 24791.0F;
constexpr float kWalkSpeed = 0.1F;
constexpr std::size_t kCarriedSlots = 36;
constexpr std::size_t kJavaPlayerSlots = 46;

// Bedrock numbers the hotbar first; Java puts it last, after the 27 of the main inventory.
std::size_t javaSlot(std::size_t bedrockSlot) { return bedrockSlot < 9 ? 36 + bedrockSlot : bedrockSlot; }

constexpr std::size_t kBlocksPerSection = 4096;
constexpr std::size_t kBiomesPerSection = 64;
constexpr std::size_t kMaxCommands = 2000;
constexpr std::chrono::milliseconds kPositionInterval{50};
// A client ends each of its ticks with this packet; vanilla refuses a second position before one arrives.
constexpr std::int32_t kClientTickEndId = 0x0d;
// Vanilla refreshes its reference position once per tick, so a move sent in the tick of the confirmation looks huge.
constexpr std::chrono::milliseconds kSettleTime{150};
// Inputs sent before the client reaches a teleport target are stale and would read as a huge step.
constexpr float kArrivalRadius = 2.0F;
constexpr std::int32_t kArrivalInputs = 100;
constexpr std::chrono::milliseconds kConnectBudget{5000};
constexpr std::chrono::seconds kJoinBudget{60};
constexpr std::chrono::seconds kSilenceBudget{45};
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
                               std::string address, std::uint16_t port, Events events,
                               std::shared_ptr<const wire::text::LangTable> language,
                               std::shared_ptr<const NativeForwarding> forwarding, Commands commands)
    : connection_(connection), backend_(std::move(backend)), address_(std::move(address)),
      commands_(std::move(commands)), port_(port), events_(std::move(events)), language_(std::move(language)),
      forwarding_(std::move(forwarding)) {}

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
    profile_ = std::move(profile);
    return openLink();
}

bool BedrockSession::openLink() {
    joinStarted_ = std::chrono::steady_clock::now();
    lastFromJava_ = joinStarted_;
    JavaLink::Events events;
    events.play = [this](std::int32_t id, std::span<const std::uint8_t> body) { fromJava(id, body); };
    events.registry = [this](const std::string& registry, std::vector<std::string> keys) {
        if (registry == "minecraft:worldgen/biome")
            biomeKeys_ = std::move(keys);
    };
    events.closed = [this](const std::string& reason) { leave(reason); };
    link_ = std::make_unique<JavaLink>(backend_, profile_, address_, port_, std::move(events), forwarding_.get(),
                                       connection_.login().xuid);
    return link_->begin(kConnectBudget);
}

bool BedrockSession::switchTo(config::BackendConfig target) {
    if (!started_ || link_ == nullptr)
        return false;
    for (const auto& [id, uuid] : others_) {
        wire::io::ByteBuffer gone;
        bedrock::writeRemoveEntity(gone, static_cast<std::int64_t>(runtimeId(id)));
        sendBedrock(BedrockPacket::RemoveEntity, gone);
        gone.clear();
        bedrock::writePlayerListRemove(gone, std::span{&uuid, 1}, connection_.protocolVersion());
        sendBedrock(BedrockPacket::PlayerList, gone);
    }
    for (const std::int32_t id : mobs_) {
        wire::io::ByteBuffer gone;
        bedrock::writeRemoveEntity(gone, static_cast<std::int64_t>(runtimeId(id)));
        sendBedrock(BedrockPacket::RemoveEntity, gone);
    }
    others_.clear();
    mobs_.clear();
    nameTags_.clear();
    waitingItems_.clear();
    for (const auto& [key, entity] : bossBars_) {
        wire::io::ByteBuffer gone;
        bedrock::writeBossEvent(gone, entity, bedrock::BedrockBossAction::Remove, "", 0.0F,
                                connection_.protocolVersion());
        sendBedrock(BedrockPacket::BossEvent, gone);
    }
    if (!sidebar_.empty()) {
        wire::io::ByteBuffer gone;
        bedrock::writeRemoveObjective(gone, sidebar_);
        sendBedrock(BedrockPacket::RemoveObjective, gone);
    }
    names_.clear();
    bossBars_.clear();
    objectives_.clear();
    sidebar_.clear();
    biomeKeys_.clear();
    inventory_ = {};
    chunks_ = 0;
    spawned_ = false;
    teleported_ = false;
    acceptedTeleport_ = -1;
    pendingMove_.reset();
    switching_ = true;
    // The client keeps a world for as long as it stays in a dimension, so it is sent to another and brought back.
    wire::io::ByteBuffer out;
    const float eye = y_ + bedrock::kPlayerEyeHeight;
    bedrock::writeChangeDimension(out, bedrock::BedrockDimension::Nether, x_, eye, z_, false);
    sendBedrock(BedrockPacket::ChangeDimension, out);
    out.clear();
    bedrock::writeChangeDimension(out, bedrock::BedrockDimension::Overworld, x_, eye, z_, false);
    sendBedrock(BedrockPacket::ChangeDimension, out);
    connection_.flush();
    clientDimension_ = 0;
    backend_ = std::move(target);
    return openLink();
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
    if (link_ == nullptr)
        return;
    link_->pump();
    const auto now = std::chrono::steady_clock::now();
    if (pendingMove_ && now - lastPosition_ >= kPositionInterval) {
        wire::io::ByteBuffer out;
        pendingMove_->write(out);
        pendingMove_.reset();
        lastPosition_ = now;
        sendJava(play::ServerboundPositionLookPacket::kId, out);
        tickEnded();
    }
    if (!spawned_ && now - joinStarted_ > kJoinBudget)
        leave("The server took too long to let you in.");
    else if (now - lastFromJava_ > kSilenceBudget)
        leave("The server stopped answering.");
}

void BedrockSession::commandList(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto tree = play::CommandsPacket::read(in);
    if (!tree || tree->rootIndex < 0 || static_cast<std::size_t>(tree->rootIndex) >= tree->nodes.size())
        return;
    std::vector<bedrock::CommandInfo> commands;
    for (const std::int32_t child : tree->nodes[static_cast<std::size_t>(tree->rootIndex)].children) {
        if (child < 0 || static_cast<std::size_t>(child) >= tree->nodes.size() || commands.size() >= kMaxCommands)
            continue;
        const auto& node = tree->nodes[static_cast<std::size_t>(child)];
        if (node.kind == play::CommandNodeData::Kind::Literal && node.name && !node.name->empty())
            commands.push_back({*node.name, {}});
    }
    if (commands_.specs) {
        for (const auto& spec : commands_.specs()) {
            if (spec.literals.empty() || commands.size() >= kMaxCommands)
                continue;
            const auto known = [&](const bedrock::CommandInfo& info) { return info.name == spec.literals.front(); };
            if (std::ranges::none_of(commands, known))
                commands.push_back({spec.literals.front(), {}});
        }
    }
    wire::io::ByteBuffer out;
    bedrock::writeAvailableCommands(out, commands);
    sendBedrock(BedrockPacket::AvailableCommands, out);
}

void BedrockSession::kicked(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::PlayDisconnectPacket::read(in);
    std::string reason = packet ? wire::text::toLegacyText(resolved(packet->reason)) : std::string{};
    if (events_ && packet)
        events_({runtime::PlayerEventKind::kKick, javaName_, backend_.name, javaUuid_,
                 wire::text::toPlainText(resolved(packet->reason))});
    leave(reason.empty() ? "The server closed the connection." : reason);
}

void BedrockSession::leave(const std::string& reason) {
    wire::common::Logger{"Bedrock"}.info([&] { return connection_.login().name + " left: " + reason; });
    if (connection_.state() != bedrock::BedrockState::Closed)
        connection_.disconnect(reason);
}

void BedrockSession::sendBedrock(BedrockPacket packet, const wire::io::ByteBuffer& body) {
    connection_.sendPacket(packet, body.readable());
}

void BedrockSession::tickEnded() { sendJava(kClientTickEndId, wire::io::ByteBuffer{}); }

void BedrockSession::sendJava(std::int32_t id, const wire::io::ByteBuffer& body) {
    if (link_ != nullptr)
        link_->send(id, body.readable());
}

void BedrockSession::fromJava(std::int32_t id, std::span<const std::uint8_t> body) {
    lastFromJava_ = std::chrono::steady_clock::now();
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
    else if (id == play::PlayerChatPacket::kId)
        playerChat(body);
    else if (id == play::PlayerCombatKillPacket::kId)
        died(body);
    else if (id == play::RespawnPacket::kId)
        respawned(body);
    else if (id == play::kSetTitleTextId)
        title(bedrock::TitleType::Title, body);
    else if (id == play::kSetSubtitleTextId)
        title(bedrock::TitleType::Subtitle, body);
    else if (id == play::kSetActionBarTextId)
        title(bedrock::TitleType::ActionBar, body);
    else if (id == play::SetTitlesAnimationPacket::kId)
        titleTimes(body);
    else if (id == play::ClearTitlesPacket::kId)
        titlesCleared(body);
    else if (id == play::SetObjectivePacket::kId)
        objective(body);
    else if (id == play::SetDisplayObjectivePacket::kId)
        displayObjective(body);
    else if (id == play::SetScorePacket::kId)
        scoreSet(body);
    else if (id == play::ResetScorePacket::kId)
        scoreReset(body);
    else if (id == play::BossEventPacket::kId)
        bossEvent(body);
    else if (id == play::SetHealthPacket::kId)
        healthChanged(body);
    else if (id == play::SetExperiencePacket::kId)
        experienceChanged(body);
    else if (id == play::SetTimePacket::kId)
        timeChanged(body);
    else if (id == play::GameEventPacket::kId)
        gameEvent(body);
    else if (id == play::ContainerSetContentPacket::kId)
        inventoryContent(body);
    else if (id == play::ContainerSetSlotPacket::kId)
        inventorySlot(body);
    else if (id == play::OpenScreenPacket::kId)
        screenOpened(body);
    else if (id == play::ContainerClosePacket::kClientboundId)
        screenClosed(body);
    else if (id == play::ClientboundBlockUpdatePacket::kId)
        blockChanged(body);
    else if (id == play::SectionBlocksUpdatePacket::kId)
        sectionChanged(body);
    else if (id == play::ChunkBatchFinishedPacket::kId)
        batchFinished();
    else if (id == play::CommandsPacket::kId)
        commandList(body);
    else if (id == play::PlayDisconnectPacket::kId)
        kicked(body);
    connection_.flush();
}

void BedrockSession::joined(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto login = play::LoginPlayPacket::read(in);
    if (!login || (started_ && !switching_))
        return;
    useDimension(dimensionOf(login->spawnInfo.dimensionName));
    if (switching_) {
        switching_ = false;
        gamemode_ = static_cast<std::int32_t>(login->spawnInfo.gamemode);
        if (dimension_ != clientDimension_)
            changeDimension();
        return;
    }
    started_ = true;
    clientDimension_ = dimension_;
    const int protocol = connection_.protocolVersion();
    sections_ = 24;

    bedrock::StartGameInfo info;
    info.dimension = dimension_;
    info.x = x_;
    info.y = y_ + bedrock::kPlayerEyeHeight;
    info.z = z_;
    selfEntity_ = login->entityId;
    gamemode_ = static_cast<std::int32_t>(login->spawnInfo.gamemode);
    info.playerGameType = gamemode_;
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
        const std::string javaType = "minecraft:" + std::string{path};
        const std::string_view name = bedrock::bedrockEntityName(javaType);
        if (path.empty() || name.empty())
            return;
        const play::EntityDimensions box = play::javaEntityDimensions(path).value_or(play::EntityDimensions{});
        wire::io::ByteBuffer out;
        bedrock::writeAddEntity(out, static_cast<std::int64_t>(runtimeId(packet->entityId)), name,
                                static_cast<float>(packet->x), static_cast<float>(packet->y),
                                static_cast<float>(packet->z), packet->yaw, packet->pitch, box.width, box.height,
                                NameTag{}.flags, connection_.protocolVersion());
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
    if (mobs_.contains(packet->entityId))
        nameTagged(*packet);
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

std::uint64_t BedrockSession::sharedFlags(std::uint8_t java) {
    using Data = play::SetEntityDataPacket;
    using Flag = bedrock::EntityFlag;
    std::uint64_t flags = 0;
    const auto map = [&](std::uint8_t javaBit, Flag flag) {
        if ((java & javaBit) != 0)
            flags |= bedrock::flagBit(flag);
    };
    map(Data::kFlagOnFire, Flag::OnFire);
    map(Data::kFlagSneaking, Flag::Sneaking);
    map(Data::kFlagSprinting, Flag::Sprinting);
    map(Data::kFlagInvisible, Flag::Invisible);
    map(Data::kFlagFallFlying, Flag::Gliding);
    return flags;
}

void BedrockSession::nameTagged(const play::SetEntityDataPacket& packet) {
    NameTag& tag = nameTags_[packet.entityId];
    bool changed = false;
    bool flagsChanged = false;
    for (const auto& entry : packet.entries) {
        if (entry.index == play::SetEntityDataPacket::kSharedFlagsIndex) {
            const auto* shared = std::get_if<play::SetEntityDataPacket::ByteValue>(&entry.value);
            if (shared == nullptr)
                continue;
            tag.flags &= ~kSharedMask;
            tag.flags |= sharedFlags(static_cast<std::uint8_t>(shared->value));
            flagsChanged = true;
        } else if (entry.index == kBabyIndex) {
            const auto* baby = std::get_if<play::SetEntityDataPacket::BoolValue>(&entry.value);
            if (baby == nullptr)
                continue;
            const auto bit = bedrock::flagBit(bedrock::EntityFlag::Baby);
            tag.flags = baby->value ? (tag.flags | bit) : (tag.flags & ~bit);
            flagsChanged = true;
        } else if (entry.index == play::SetEntityDataPacket::kCustomNameIndex) {
            const auto* name = std::get_if<play::SetEntityDataPacket::OptionalTextValue>(&entry.value);
            if (name == nullptr)
                continue;
            tag.text = name->value ? wire::text::toLegacyText(resolved(*name->value)) : std::string{};
            changed = true;
        } else if (entry.index == play::SetEntityDataPacket::kCustomNameVisibleIndex) {
            const auto* visible = std::get_if<play::SetEntityDataPacket::BoolValue>(&entry.value);
            if (visible == nullptr)
                continue;
            tag.always = visible->value;
            changed = true;
        }
    }
    if (flagsChanged) {
        wire::io::ByteBuffer out;
        bedrock::writeEntityFlags(out, runtimeId(packet.entityId), tag.flags, connection_.protocolVersion());
        sendBedrock(BedrockPacket::SetEntityData, out);
    }
    if (!changed)
        return;
    wire::io::ByteBuffer out;
    bedrock::writeNameTag(out, runtimeId(packet.entityId), tag.text, tag.always, connection_.protocolVersion());
    sendBedrock(BedrockPacket::SetEntityData, out);
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
        nameTags_.erase(id);
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
    const std::string javaItem = "minecraft:" + std::string{path};
    const bedrock::ItemName* named = bedrock::bedrockItem(javaItem);
    if (named == nullptr)
        return {};
    return {.runtimeId = named->palettePlace + 1, .count = item->count, .data = named->data, .blockRuntimeId = 0};
}

std::int32_t BedrockSession::dimensionOf(std::string_view worldKey) {
    if (worldKey.find("nether") != std::string_view::npos)
        return 1;
    if (worldKey.find("the_end") != std::string_view::npos)
        return 2;
    return 0;
}

void BedrockSession::useDimension(std::int32_t dimension) {
    dimension_ = dimension;
    // The overworld starts four sections below zero; the other two start at the bottom.
    minSection_ = dimension == 0 ? kOverworldMinSection : 0;
}

void BedrockSession::changeDimension() {
    wire::io::ByteBuffer out;
    const float eye = y_ + bedrock::kPlayerEyeHeight;
    bedrock::writeChangeDimension(out, static_cast<bedrock::BedrockDimension>(dimension_), x_, eye, z_, false);
    sendBedrock(BedrockPacket::ChangeDimension, out);
    clientDimension_ = dimension_;
}

void BedrockSession::respawned(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::RespawnPacket::read(in);
    respawning_ = true;
    if (!packet || dimensionOf(packet->spawnInfo.dimensionName) == dimension_)
        return;
    for (const auto& [id, uuid] : others_) {
        wire::io::ByteBuffer gone;
        bedrock::writeRemoveEntity(gone, static_cast<std::int64_t>(runtimeId(id)));
        sendBedrock(BedrockPacket::RemoveEntity, gone);
        gone.clear();
        bedrock::writePlayerListRemove(gone, std::span{&uuid, 1}, connection_.protocolVersion());
        sendBedrock(BedrockPacket::PlayerList, gone);
    }
    for (const std::int32_t id : mobs_) {
        wire::io::ByteBuffer gone;
        bedrock::writeRemoveEntity(gone, static_cast<std::int64_t>(runtimeId(id)));
        sendBedrock(BedrockPacket::RemoveEntity, gone);
    }
    others_.clear();
    mobs_.clear();
    nameTags_.clear();
    waitingItems_.clear();
    chunks_ = 0;
    useDimension(dimensionOf(packet->spawnInfo.dimensionName));
    changeDimension();
}

void BedrockSession::died(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::PlayerCombatKillPacket::read(in);
    if (!packet)
        return;
    wire::io::ByteBuffer out;
    bedrock::writeDeathInfo(out, wire::text::toLegacyText(resolved(packet->message)));
    sendBedrock(BedrockPacket::DeathInfo, out);
}

void BedrockSession::playerChat(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::PlayerChatPacket::read(in);
    if (!packet)
        return;
    const auto known = names_.find(packet->sender.toString());
    const std::string name =
        known != names_.end() ? known->second : wire::text::toPlainText(resolved(packet->chatType.name));
    const std::string message =
        packet->unsignedContent ? wire::text::toLegacyText(resolved(*packet->unsignedContent)) : packet->content;
    wire::io::ByteBuffer out;
    bedrock::writeText(out, bedrock::TextType::Chat, name, message, "");
    sendBedrock(BedrockPacket::Text, out);
}

void BedrockSession::title(bedrock::TitleType type, std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::TitleTextPacket::read(in);
    if (!packet)
        return;
    wire::io::ByteBuffer out;
    bedrock::writeSetTitle(out, type, wire::text::toLegacyText(resolved(packet->text)), kTitleKeep, kTitleKeep,
                           kTitleKeep);
    sendBedrock(BedrockPacket::SetTitle, out);
}

void BedrockSession::titleTimes(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetTitlesAnimationPacket::read(in);
    if (!packet)
        return;
    wire::io::ByteBuffer out;
    bedrock::writeSetTitle(out, bedrock::TitleType::Times, "", packet->fadeIn, packet->stay, packet->fadeOut);
    sendBedrock(BedrockPacket::SetTitle, out);
}

void BedrockSession::titlesCleared(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::ClearTitlesPacket::read(in);
    if (!packet)
        return;
    wire::io::ByteBuffer out;
    bedrock::writeSetTitle(out, packet->resetTimes ? bedrock::TitleType::Reset : bedrock::TitleType::Clear, "", 0, 0,
                           0);
    sendBedrock(BedrockPacket::SetTitle, out);
}

wire::text::Component BedrockSession::resolved(const wire::text::Component& component) const {
    return language_ != nullptr ? wire::text::resolveComponent(component, *language_) : component;
}

std::string BedrockSession::sidebarTitle() const {
    const auto found = objectives_.find(sidebar_);
    return found == objectives_.end() ? sidebar_ : found->second.title;
}

void BedrockSession::drawSidebar() {
    if (sidebar_.empty())
        return;
    wire::io::ByteBuffer out;
    bedrock::writeSetDisplayObjective(out, "sidebar", sidebar_, sidebarTitle());
    sendBedrock(BedrockPacket::SetDisplayObjective, out);
    std::vector<bedrock::BedrockScoreLine> lines;
    for (const auto& [owner, line] : objectives_[sidebar_].lines)
        lines.push_back({line.id, line.text, line.score});
    if (lines.empty())
        return;
    out.clear();
    bedrock::writeSetScore(out, sidebar_, lines, connection_.protocolVersion());
    sendBedrock(BedrockPacket::SetScore, out);
}

void BedrockSession::objective(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetObjectivePacket::read(in);
    if (!packet)
        return;
    if (packet->method == play::SetObjectivePacket::kMethodRemove) {
        if (packet->objectiveName == sidebar_) {
            wire::io::ByteBuffer out;
            bedrock::writeRemoveObjective(out, sidebar_);
            sendBedrock(BedrockPacket::RemoveObjective, out);
            sidebar_.clear();
        }
        objectives_.erase(packet->objectiveName);
        return;
    }
    Objective& stored = objectives_[packet->objectiveName];
    if (packet->displayName)
        stored.title = wire::text::toLegacyText(resolved(*packet->displayName));
    if (packet->objectiveName == sidebar_)
        drawSidebar();
}

void BedrockSession::displayObjective(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetDisplayObjectivePacket::read(in);
    if (!packet || packet->slot != play::DisplaySlot::Sidebar)
        return;
    if (!sidebar_.empty() && sidebar_ != packet->objectiveName) {
        wire::io::ByteBuffer out;
        bedrock::writeRemoveObjective(out, sidebar_);
        sendBedrock(BedrockPacket::RemoveObjective, out);
    }
    sidebar_ = packet->objectiveName;
    drawSidebar();
}

void BedrockSession::scoreSet(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetScorePacket::read(in);
    if (!packet)
        return;
    Objective& stored = objectives_[packet->objectiveName];
    auto [line, added] = stored.lines.try_emplace(packet->owner);
    if (added)
        line->second.id = ++nextLine_;
    line->second.text = packet->display ? wire::text::toLegacyText(resolved(*packet->display)) : packet->owner;
    line->second.score = packet->score;
    if (packet->objectiveName != sidebar_)
        return;
    const bedrock::BedrockScoreLine drawn{line->second.id, line->second.text, line->second.score};
    wire::io::ByteBuffer out;
    bedrock::writeSetScore(out, sidebar_, std::span{&drawn, 1}, connection_.protocolVersion());
    sendBedrock(BedrockPacket::SetScore, out);
}

void BedrockSession::scoreReset(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::ResetScorePacket::read(in);
    if (!packet)
        return;
    for (auto& [name, stored] : objectives_) {
        if (packet->objectiveName && *packet->objectiveName != name)
            continue;
        const auto line = stored.lines.find(packet->owner);
        if (line == stored.lines.end())
            continue;
        if (name == sidebar_) {
            const std::int64_t id = line->second.id;
            wire::io::ByteBuffer out;
            bedrock::writeRemoveScores(out, name, std::span{&id, 1}, connection_.protocolVersion());
            sendBedrock(BedrockPacket::SetScore, out);
        }
        stored.lines.erase(line);
    }
}

void BedrockSession::bossEvent(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::BossEventPacket::read(in);
    if (!packet)
        return;
    const std::string key = packet->id.toString();
    wire::io::ByteBuffer out;
    if (const auto* add = std::get_if<play::BossEventPacket::Add>(&packet->operation)) {
        const std::int64_t entity = kBossBase + static_cast<std::int64_t>(bossBars_.size()) + 1;
        bossBars_[key] = entity;
        bedrock::writeBossEvent(out, entity, bedrock::BedrockBossAction::Create,
                                wire::text::toLegacyText(resolved(add->name)), add->progress,
                                connection_.protocolVersion());
        sendBedrock(BedrockPacket::BossEvent, out);
        return;
    }
    const auto known = bossBars_.find(key);
    if (known == bossBars_.end())
        return;
    if (std::holds_alternative<play::BossEventPacket::Remove>(packet->operation)) {
        bedrock::writeBossEvent(out, known->second, bedrock::BedrockBossAction::Remove, "", 0.0F,
                                connection_.protocolVersion());
        bossBars_.erase(known);
    } else if (const auto* progress = std::get_if<play::BossEventPacket::UpdateProgress>(&packet->operation)) {
        bedrock::writeBossEvent(out, known->second, bedrock::BedrockBossAction::UpdatePercentage, "",
                                progress->progress, connection_.protocolVersion());
    } else if (const auto* name = std::get_if<play::BossEventPacket::UpdateName>(&packet->operation)) {
        bedrock::writeBossEvent(out, known->second, bedrock::BedrockBossAction::UpdateName,
                                wire::text::toLegacyText(resolved(name->name)), 0.0F, connection_.protocolVersion());
    } else {
        return;
    }
    sendBedrock(BedrockPacket::BossEvent, out);
}

void BedrockSession::healthChanged(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetHealthPacket::read(in);
    if (!packet)
        return;
    const std::array<bedrock::AttributeValue, 4> values{{
        {"minecraft:health", 0.0F, kMaxHealth, packet->health, kMaxHealth},
        {"minecraft:player.hunger", 0.0F, kMaxFood, static_cast<float>(packet->food), kMaxFood},
        {"minecraft:player.saturation", 0.0F, kMaxFood, packet->saturation, kMaxFood},
        // The client never told how fast it walks does it at no pace.
        {"minecraft:movement", 0.0F, 1024.0F, kWalkSpeed, kWalkSpeed},
    }};
    wire::io::ByteBuffer out;
    bedrock::writeUpdateAttributes(out, 1, values, 0);
    sendBedrock(BedrockPacket::UpdateAttributes, out);
}

void BedrockSession::experienceChanged(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetExperiencePacket::read(in);
    if (!packet)
        return;
    const std::array<bedrock::AttributeValue, 2> values{{
        {"minecraft:player.experience", 0.0F, 1.0F, packet->progress, 0.0F},
        {"minecraft:player.level", 0.0F, kMaxLevel, static_cast<float>(packet->level), 0.0F},
    }};
    wire::io::ByteBuffer out;
    bedrock::writeUpdateAttributes(out, 1, values, 0);
    sendBedrock(BedrockPacket::UpdateAttributes, out);
}

void BedrockSession::timeChanged(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetTimePacket::read(in);
    if (!packet)
        return;
    for (const auto& clock : packet->clocks) {
        if (clock.clockId != play::SetTimePacket::kOverworldClock)
            continue;
        wire::io::ByteBuffer out;
        bedrock::writeSetTime(out, static_cast<std::int32_t>(clock.totalTicks % kTicksPerDay));
        sendBedrock(BedrockPacket::SetTime, out);
        return;
    }
}

void BedrockSession::applyGamemode(std::int32_t mode) {
    constexpr std::int32_t kCreative = 1;
    constexpr std::int32_t kAdventure = 2;
    constexpr std::int32_t kSpectator = 3;
    wire::io::ByteBuffer settings;
    bedrock::writeUpdateAdventureSettings(settings, mode == kAdventure || mode == kSpectator);
    sendBedrock(BedrockPacket::UpdateAdventureSettings, settings);
    wire::io::ByteBuffer abilities;
    bedrock::writeUpdateAbilities(abilities, 1, false, mode == kCreative);
    sendBedrock(BedrockPacket::UpdateAbilities, abilities);
}

void BedrockSession::gameEvent(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::GameEventPacket::read(in);
    if (!packet)
        return;
    using Level = bedrock::LevelEventType;
    const auto send = [this](Level type, float strength) {
        wire::io::ByteBuffer out;
        bedrock::writeLevelEvent(
            out, type, 0.0F, 0.0F, 0.0F,
            static_cast<std::int32_t>(strength * static_cast<float>(bedrock::kWeatherStrengthScale)));
        sendBedrock(BedrockPacket::LevelEvent, out);
    };
    if (packet->event == play::GameEventPacket::kChangeGameMode) {
        applyGamemode(static_cast<std::int32_t>(packet->param));
        wire::io::ByteBuffer mode;
        bedrock::writeSetPlayerGameType(mode, static_cast<std::int32_t>(packet->param));
        sendBedrock(BedrockPacket::SetPlayerGameType, mode);
        return;
    }
    if (packet->event == play::GameEventPacket::kStartRaining)
        send(Level::StartRaining, 1.0F);
    else if (packet->event == play::GameEventPacket::kStopRaining)
        send(Level::StopRaining, 0.0F);
    else if (packet->event == play::GameEventPacket::kRainLevelChange)
        send(packet->param > 0.0F ? Level::StartRaining : Level::StopRaining, packet->param);
    else if (packet->event == play::GameEventPacket::kThunderLevelChange)
        send(packet->param > 0.0F ? Level::StartThunderstorm : Level::StopThunderstorm, packet->param);
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

std::optional<bedrock::BedrockContainerType> BedrockSession::windowType(std::int32_t menu, std::size_t& slots) {
    using Type = bedrock::BedrockContainerType;
    constexpr std::int32_t kGeneric9x1 = 0;
    constexpr std::int32_t kGeneric9x6 = 5;
    constexpr std::int32_t kGeneric3x3 = 6;
    constexpr std::int32_t kHopper = 16;
    constexpr std::size_t kRow = 9;
    if (menu >= kGeneric9x1 && menu <= kGeneric9x6) {
        slots = kRow * static_cast<std::size_t>(menu + 1);
        return Type::Container;
    }
    if (menu == kGeneric3x3) {
        slots = kRow;
        return Type::Dispenser;
    }
    if (menu == kHopper) {
        slots = 5;
        return Type::Hopper;
    }
    return std::nullopt;
}

void BedrockSession::screenOpened(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::OpenScreenPacket::read(in);
    if (!packet)
        return;
    if (window_)
        closeWindow(true);
    std::size_t slots = 0;
    const auto type = windowType(packet->menuType, slots);
    if (!type) {
        // Nothing to draw for this menu: tell the backend the player closed it so it does not stay open for nobody.
        wire::io::ByteBuffer out;
        play::ContainerClosePacket{packet->containerId}.write(out);
        sendJava(play::ContainerClosePacket::kServerboundId, out);
        return;
    }
    window_ = Window{packet->containerId, *type, slots, {}, false};
    window_->items.resize(slots);
}

void BedrockSession::windowContent(const play::ContainerSetContentPacket& packet) {
    for (std::size_t slot = 0; slot < window_->items.size() && slot < packet.items.size(); ++slot)
        window_->items[slot] = packet.items[slot];
    if (!window_->shown) {
        window_->shown = true;
        wire::io::ByteBuffer open;
        bedrock::writeContainerOpen(open, window_->id, window_->type, lastUse_[0], lastUse_[1], lastUse_[2]);
        sendBedrock(BedrockPacket::ContainerOpen, open);
    }
    drawWindow();
}

void BedrockSession::drawWindow() {
    std::vector<bedrock::BedrockItemStack> items;
    items.reserve(window_->items.size());
    for (const auto& item : window_->items)
        items.push_back(stackOf(item));
    wire::io::ByteBuffer out;
    bedrock::writeInventoryContent(out, static_cast<std::uint32_t>(window_->id), items,
                                   bedrock::BedrockContainerSlot::LevelEntity, connection_.protocolVersion());
    sendBedrock(BedrockPacket::InventoryContent, out);
}

void BedrockSession::screenClosed(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::ContainerClosePacket::read(in);
    if (packet && window_ && packet->containerId == window_->id)
        closeWindow(false);
}

void BedrockSession::closeWindow(bool tellBackend) {
    if (!window_)
        return;
    if (window_->shown) {
        wire::io::ByteBuffer out;
        bedrock::writeContainerClose(out, window_->id, window_->type, !tellBackend);
        sendBedrock(BedrockPacket::ContainerClose, out);
    }
    if (tellBackend) {
        wire::io::ByteBuffer out;
        play::ContainerClosePacket{window_->id}.write(out);
        sendJava(play::ContainerClosePacket::kServerboundId, out);
    }
    window_.reset();
}

std::optional<std::int32_t> BedrockSession::clickSlot(const bedrock::BedrockStackSlot& slot) const {
    using Place = bedrock::BedrockContainerSlot;
    const auto container = static_cast<Place>(slot.container);
    const std::int32_t slots = window_ ? static_cast<std::int32_t>(window_->slots) : 0;
    if (container == Place::LevelEntity)
        return window_ && slot.slot >= 0 && slot.slot < slots ? std::optional{slot.slot} : std::nullopt;
    const bool carried =
        container == Place::HotbarAndInventory || container == Place::Hotbar || container == Place::Inventory;
    if (!carried || slot.slot < 0 || slot.slot >= static_cast<std::int32_t>(kCarriedSlots))
        return std::nullopt;
    if (!window_)
        return static_cast<std::int32_t>(javaSlot(static_cast<std::size_t>(slot.slot)));
    return slot.slot < 9 ? slots + 27 + slot.slot : slots + slot.slot - 9;
}

const std::optional<play::WireItemStack>& BedrockSession::itemAt(std::int32_t javaSlot) const {
    static const std::optional<play::WireItemStack> kNothing;
    const auto index = static_cast<std::size_t>(javaSlot);
    if (!window_)
        return index < inventory_.size() ? inventory_[index] : kNothing;
    const std::size_t slots = window_->slots;
    if (index < slots)
        return index < window_->items.size() ? window_->items[index] : kNothing;
    const std::size_t own = index - slots;
    const std::size_t player = own < 27 ? own + 9 : own - 27 + 36;
    return player < inventory_.size() ? inventory_[player] : kNothing;
}

void BedrockSession::click(std::int32_t slot, std::int8_t button, play::ServerboundContainerClickPacket::Mode mode) {
    play::ServerboundContainerClickPacket packet;
    packet.containerId = window_ ? window_->id : 0;
    packet.stateId = stateId_;
    packet.slot = static_cast<std::int16_t>(slot);
    packet.button = button;
    packet.mode = mode;
    wire::io::ByteBuffer out;
    packet.write(out);
    sendJava(play::ServerboundContainerClickPacket::kId, out);
}

bool BedrockSession::clickFor(const bedrock::BedrockStackAction& action) {
    using Type = bedrock::BedrockStackActionType;
    using Mode = play::ServerboundContainerClickPacket::Mode;
    const auto cursor = static_cast<std::int32_t>(bedrock::BedrockContainerSlot::Cursor);
    const auto source = clickSlot(action.source);
    const auto destination = clickSlot(action.destination);
    if (action.type == Type::Take && source && action.destination.container == cursor) {
        const std::int32_t held = itemAt(*source) ? itemAt(*source)->count : 0;
        click(*source, action.count > 0 && action.count < held ? 1 : 0, Mode::Pickup);
        return true;
    }
    if (action.type == Type::Place && action.source.container == cursor && destination) {
        click(*destination, action.count == 1 ? 1 : 0, Mode::Pickup);
        return true;
    }
    if (action.type == Type::Swap && source && destination) {
        click(*source, 0, Mode::Pickup);
        click(*destination, 0, Mode::Pickup);
        click(*source, 0, Mode::Pickup);
        return true;
    }
    if (action.type == Type::Swap && source && action.destination.container == cursor) {
        click(*source, 0, Mode::Pickup);
        return true;
    }
    if (action.type == Type::Swap && action.source.container == cursor && destination) {
        click(*destination, 0, Mode::Pickup);
        return true;
    }
    return false;
}

void BedrockSession::stackRequests(wire::io::ByteBuffer& body) {
    for (const auto& request : bedrock::readItemStackRequests(body, connection_.protocolVersion()))
        stackRequest(request);
}

void BedrockSession::stackRequest(const bedrock::BedrockStackRequest& request) {
    bool known = !request.actions.empty();
    for (const auto& action : request.actions)
        known = known && clickFor(action);
    if (!known) {
        rejectStackRequest(request.requestId);
        return;
    }
    wire::io::ByteBuffer out;
    bedrock::writeItemStackResponse(out, request.requestId, true, {}, connection_.protocolVersion());
    sendBedrock(BedrockPacket::ItemStackResponse, out);
}

void BedrockSession::rejectStackRequest(std::int32_t requestId) {
    wire::io::ByteBuffer out;
    bedrock::writeItemStackResponse(out, requestId, false, {}, connection_.protocolVersion());
    sendBedrock(BedrockPacket::ItemStackResponse, out);
}

void BedrockSession::inventoryContent(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::ContainerSetContentPacket::read(in);
    if (packet)
        stateId_ = packet->stateId;
    if (packet && window_ && packet->containerId == window_->id) {
        windowContent(*packet);
        return;
    }
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
    if (packet && window_ && packet->containerId == window_->id) {
        if (packet->slot >= 0 && static_cast<std::size_t>(packet->slot) < window_->items.size()) {
            window_->items[static_cast<std::size_t>(packet->slot)] = packet->item;
            drawWindow();
        }
        return;
    }
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
    wire::io::ByteBuffer data;
    data.writeBytes(packet->chunkData);
    std::vector<std::array<std::int32_t, kBlocksPerSection>> blocks;
    std::vector<bedrock::BiomeQuarts> biomes;
    std::int32_t sent = 0;
    for (std::int32_t section = 0; section < sections_ && data.readableBytes() != 0; ++section) {
        static_cast<void>(data.readShort());
        static_cast<void>(data.readShort());
        const std::vector<std::int32_t> states =
            play::palettedContainer::read(data, kBlocksPerSection, play::palettedContainer::kBlockMaxIndirectBits);
        const std::vector<std::int32_t> quarts =
            play::palettedContainer::read(data, kBiomesPerSection, play::palettedContainer::kBiomeMaxIndirectBits);
        if (data.failed() || states.size() != kBlocksPerSection || quarts.size() != kBiomesPerSection)
            return;
        if (std::ranges::any_of(states, [](std::int32_t state) { return state != 0; }))
            sent = section + 1;
        std::copy(states.begin(), states.end(), blocks.emplace_back().begin());
        bedrock::BiomeQuarts& mapped = biomes.emplace_back();
        for (std::size_t quart = 0; quart < kBiomesPerSection; ++quart) {
            const auto id = static_cast<std::size_t>(quarts[quart]);
            const auto found = id < biomeKeys_.size() ? bedrock::bedrockBiomeId(biomeKeys_[id]) : std::nullopt;
            mapped[quart] = found.value_or(bedrock::kFallbackBedrockBiome);
        }
    }
    bedrock::ColumnShape shape;
    shape.chunkX = packet->x;
    shape.chunkZ = packet->z;
    shape.minSection = minSection_;
    shape.dimension = dimension_;
    shape.sections = static_cast<std::int32_t>(blocks.size());
    shape.sent = sent;
    shape.protocol = connection_.protocolVersion();
    const std::vector<std::uint8_t> column = bedrock::encodeColumn(
        shape,
        [&blocks](std::int32_t section, std::span<std::int32_t> into) {
            std::ranges::copy(blocks[static_cast<std::size_t>(section)], into.begin());
        },
        [&biomes](std::int32_t section, bedrock::BiomeQuarts& quarts) {
            quarts = biomes[static_cast<std::size_t>(section)];
        });
    wire::io::ByteBuffer out;
    out.writeBytes(column);
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
    bedrock::writeNetworkChunkPublisherUpdate(out, static_cast<std::int32_t>(x_), static_cast<std::int32_t>(y_),
                                              static_cast<std::int32_t>(z_), kInitialRadius * 16);
    sendBedrock(BedrockPacket::NetworkChunkPublisherUpdate, out);
    out.clear();
    if (respawning_) {
        respawning_ = false;
        wire::io::ByteBuffer ready;
        bedrock::writeRespawn(ready, x_, y_ + bedrock::kPlayerEyeHeight, z_, bedrock::RespawnState::ServerReady, 1);
        sendBedrock(BedrockPacket::Respawn, ready);
    }
    bedrock::writeTeleport(out, 1, x_, y_ + bedrock::kPlayerEyeHeight, z_, yaw_, pitch_, 0,
                           connection_.protocolVersion());
    sendBedrock(BedrockPacket::MovePlayer, out);
    out.clear();
    if (position->teleportId != acceptedTeleport_) {
        acceptedTeleport_ = position->teleportId;
        holdPositionsUntil_ = std::chrono::steady_clock::now() + kSettleTime;
        play::AcceptTeleportationPacket{.teleportId = position->teleportId,
                                        .x = position->x,
                                        .y = position->y,
                                        .z = position->z,
                                        .yaw = yaw_,
                                        .pitch = pitch_}
            .write(out);
        sendJava(play::AcceptTeleportationPacket::kId, out);
        out.clear();
        sendJava(play::kPlayerLoadedId, out);
    }
    teleported_ = true;
    arriving_ = true;
    arrivalInputs_ = 0;
    spawn();
}

void BedrockSession::spawn() {
    if (spawned_ || !teleported_ || chunks_ == 0)
        return;
    spawned_ = true;
    applyGamemode(gamemode_);
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
    const std::string shown = wire::text::toLegacyText(resolved(packet->content));
    if (packet->isActionBar) {
        bedrock::writeSetTitle(out, bedrock::TitleType::ActionBar, shown, kTitleKeep, kTitleKeep, kTitleKeep);
        sendBedrock(BedrockPacket::SetTitle, out);
        return;
    }
    bedrock::writeText(out, bedrock::TextType::Raw, "", shown, "");
    sendBedrock(BedrockPacket::Text, out);
}

void BedrockSession::sprinting(bool now) {
    if (now == sprinting_)
        return;
    sprinting_ = now;
    play::PlayerCommandPacket command;
    command.entityId = selfEntity_;
    command.action =
        now ? play::PlayerCommandPacket::Action::StartSprinting : play::PlayerCommandPacket::Action::StopSprinting;
    wire::io::ByteBuffer out;
    command.write(out);
    sendJava(play::PlayerCommandPacket::kId, out);
}

void BedrockSession::entityTransaction(wire::io::ByteBuffer& body) {
    const bedrock::BedrockTransaction transaction = bedrock::readTransaction(body, connection_.protocolVersion());
    if (!transaction.entityUse || transaction.entityUse->runtimeEntityId < kOthersBase)
        return;
    const auto target = static_cast<std::int32_t>(transaction.entityUse->runtimeEntityId - kOthersBase);
    wire::io::ByteBuffer out;
    if (transaction.entityUse->action == kAttackAction) {
        play::AttackPacket{target}.write(out);
        sendJava(play::AttackPacket::kId, out);
        return;
    }
    play::InteractPacket interact;
    interact.entityId = target;
    interact.write(out);
    sendJava(play::InteractPacket::kId, out);
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
    lastUse_ = {use.x, use.y, use.z};
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
        if (input && input->stackRequest)
            stackRequest(*input->stackRequest);
        if (!input || !teleported_)
            return;
        if (arriving_) {
            const float dx = input->x - x_;
            const float dy = input->y - bedrock::kPlayerEyeHeight - y_;
            const float dz = input->z - z_;
            if (dx * dx + dy * dy + dz * dz > kArrivalRadius * kArrivalRadius && ++arrivalInputs_ < kArrivalInputs)
                return;
            arriving_ = false;
        }
        wire::io::ByteBuffer out;
        play::ServerboundPositionLookPacket move;
        move.x = static_cast<double>(input->x);
        move.y = static_cast<double>(input->y) - static_cast<double>(bedrock::kPlayerEyeHeight);
        move.z = static_cast<double>(input->z);
        move.yaw = input->yaw;
        move.pitch = input->pitch;
        move.onGround = input->onGround;
        const auto now = std::chrono::steady_clock::now();
        if (now < holdPositionsUntil_) {
            pendingMove_.reset();
        } else if (now - lastPosition_ < kPositionInterval) {
            pendingMove_ = move;
        } else {
            lastPosition_ = now;
            pendingMove_.reset();
            move.write(out);
            sendJava(play::ServerboundPositionLookPacket::kId, out);
            tickEnded();
        }
        sprinting(input->sprinting);
        interact(*input);
    } else if (packet == BedrockPacket::ContainerClose) {
        if (const auto closed = bedrock::readContainerClose(body);
            closed && window_ && closed->containerId == window_->id)
            closeWindow(true);
    } else if (packet == BedrockPacket::ItemStackRequest) {
        stackRequests(body);
    } else if (packet == BedrockPacket::InventoryTransaction) {
        entityTransaction(body);
    } else if (packet == BedrockPacket::Animate) {
        const auto animate = bedrock::readAnimate(body);
        if (animate && animate->action == kSwingAction) {
            wire::io::ByteBuffer out;
            wire::protocol::varInt::write(out, play::InteractPacket::kMainHand);
            sendJava(play::kPunchId, out);
        }
    } else if (packet == BedrockPacket::Respawn) {
        if (bedrock::readRespawn(body) == bedrock::RespawnState::ClientReady) {
            wire::io::ByteBuffer out;
            play::ClientCommandPacket{play::ClientCommandPacket::Action::PerformRespawn}.write(out);
            sendJava(play::ClientCommandPacket::kId, out);
        }
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
        if (commands_.run && commands_.run(javaName_, typed.command))
            return;
        typed.write(out);
        sendJava(kChatCommandId, out);
    }
}

} // namespace basalt::proxy::runtime::detail
