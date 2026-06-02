#include "BedrockSession.hpp"

#include "../NativeForwarding.hpp"
#include "BedrockSessionLimits.hpp"

#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/bedrock/BedrockBiomes.hpp>
#include <basalt/wire/bedrock/BedrockColumn.hpp>
#include <basalt/wire/bedrock/BedrockEffects.hpp>
#include <basalt/wire/bedrock/BedrockEntityIdentifiers.hpp>
#include <basalt/wire/bedrock/BedrockItems.hpp>
#include <basalt/wire/bedrock/BedrockPackets.hpp>
#include <basalt/wire/bedrock/BedrockSounds.hpp>
#include <basalt/wire/bedrock/BedrockTypes.hpp>
#include <basalt/wire/common/SafeInt.hpp>
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
#include <basalt/wire/protocol/play/MapPackets.hpp>
#include <basalt/wire/protocol/play/PalettedContainer.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/play/PlayerStatePackets.hpp>
#include <basalt/wire/protocol/play/SimplePackets.hpp>
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
using namespace bedrockLimits;

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
    events.relay = [this](std::span<const std::uint8_t> packet) { relayFromBackend(packet); };
    link_ = std::make_unique<JavaLink>(backend_, profile_, address_, port_, std::move(events), forwarding_.get(),
                                       connection_.login().xuid);
    delegated_ = forwarding_ != nullptr && forwarding_->delegateBedrock &&
                 backend_.forwarding == wire::auth::forwarding::ForwardingMode::Basalt;
    if (delegated_) {
        const bedrock::LoginRequest& login = connection_.login();
        wire::protocol::sessionEnvelope::BedrockIdentity identity;
        identity.gamertag = login.name;
        identity.xuid = login.xuid;
        identity.authenticated = login.authenticated;
        identity.address = address_;
        identity.skinId = login.skinId;
        identity.resourcePatch = login.skinGeometry;
        identity.skinWidth = static_cast<std::int32_t>(login.skinWidth);
        identity.skinHeight = static_cast<std::int32_t>(login.skinHeight);
        identity.skinPixels = login.skinPixels;
        link_->delegateBedrock(std::move(identity), connection_.protocolVersion());
    }
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
    for (const std::int32_t effect : effects_) {
        wire::io::ByteBuffer gone;
        bedrock::writeMobEffect(gone, kSelfRuntimeId, bedrock::BedrockEffectEvent::Remove, effect, 0, false, 0, false);
        sendBedrock(BedrockPacket::MobEffect, gone);
    }
    effects_.clear();
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
    sneaking_ = false;
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
    if (!delegated_ && !spawned_ && now - joinStarted_ > kJoinBudget)
        leave("The server took too long to let you in.");
    else if (now - lastFromJava_ > kSilenceBudget)
        leave("The server stopped answering.");
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
    else if (id == play::MapItemDataPacket::kId)
        mapChanged(body);
    else if (id == play::ClientboundBlockUpdatePacket::kId)
        blockChanged(body);
    else if (id == play::SectionBlocksUpdatePacket::kId)
        sectionChanged(body);
    else if (id == play::ChunkBatchFinishedPacket::kId)
        batchFinished();
    else if (id == play::SoundPacket::kId)
        sound(body);
    else if (id == play::CustomPayloadPacket::kClientboundId)
        pluginMessage(body);
    else if (id == play::HurtAnimationPacket::kId)
        hurt(body);
    else if (id == play::EntityEventPacket::kId)
        entityEvent(body);
    else if (id == play::UpdateMobEffectPacket::kId)
        effectGiven(body);
    else if (id == play::RemoveMobEffectPacket::kId)
        effectRemoved(body);
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
    bedrock::writeNetworkChunkPublisherUpdate(out, wire::common::safeInt32(x_), wire::common::safeInt32(y_),
                                              wire::common::safeInt32(z_), kInitialRadius * 16);
    sendBedrock(BedrockPacket::NetworkChunkPublisherUpdate, out);
}

std::uint64_t BedrockSession::runtimeId(std::int32_t javaId) {
    return static_cast<std::uint64_t>(javaId) + kOthersBase;
}

} // namespace basalt::proxy::runtime::detail
