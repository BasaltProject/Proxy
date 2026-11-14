#include "BedrockSession.hpp"
#include "BedrockSessionLimits.hpp"

#include <basalt/wire/bedrock/BedrockPackets.hpp>
#include <basalt/wire/common/SafeInt.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/protocol/play/InventoryPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>

#include <cmath>

namespace basalt::proxy::runtime::detail {
namespace {

namespace bedrock = wire::bedrock;
namespace play = wire::protocol::play;
using bedrock::BedrockPacket;
using namespace bedrockLimits;

// parity: Geyser's dimension switch; the client waits for the columns around it before it lets go of the screen.
constexpr std::int32_t kEmptyColumnRadius = 3;
constexpr std::int32_t kOverworld = 0;
constexpr std::int32_t kNether = 1;

[[nodiscard]] std::uint32_t biomeSections(std::int32_t dimension) {
    constexpr std::uint32_t kOverworldSections = 24;
    constexpr std::uint32_t kNetherSections = 8;
    constexpr std::uint32_t kEndSections = 16;
    if (dimension == kOverworld)
        return kOverworldSections;
    return dimension == kNether ? kNetherSections : kEndSections;
}

} // namespace

bool BedrockSession::switchTo(config::BackendConfig target) {
    if (!started_ || link_ == nullptr || delegated_ || delegates(target))
        return false;
    if (target.name == backend_.name) {
        pending_.reset();
        tell("<gray>You are already on " + target.name + ".</gray>");
        return true;
    }
    pending_ = makeLink(target);
    pendingSerial_ = serials_;
    pendingBackend_ = std::move(target);
    pendingBiomes_.clear();
    pendingFailure_.reset();
    pendingSince_ = std::chrono::steady_clock::now();
    if (pending_->begin(kConnectBudget))
        return true;
    pending_.reset();
    return false;
}

void BedrockSession::pendingPlay(std::int32_t id, std::span<const std::uint8_t> body) {
    if (id != play::LoginPlayPacket::kId)
        return;
    commitSwitch();
    fromJava(id, body);
}

void BedrockSession::pumpPending() {
    if (pending_ == nullptr)
        return;
    pending_->pump();
    if (pendingFailure_)
        abandonSwitch(*pendingFailure_);
    else if (pending_ != nullptr && std::chrono::steady_clock::now() - pendingSince_ > kJoinBudget)
        abandonSwitch("the server took too long to let you in");
}

void BedrockSession::abandonSwitch(const std::string& reason) {
    wire::common::Logger{"Bedrock"}.info(
        [&] { return javaName_ + " stays on " + backend_.name + ": " + pendingBackend_.name + " failed, " + reason; });
    pending_.reset();
    pendingFailure_.reset();
    tell("<red>Could not connect you to " + pendingBackend_.name + ".</red>");
}

void BedrockSession::commitSwitch() {
    forgetBackend();
    hopTo(clientDimension_ == kOverworld ? kNether : kOverworld);
    link_ = std::move(pending_);
    linkSerial_ = pendingSerial_;
    backend_ = std::move(pendingBackend_);
    biomeKeys_ = std::move(pendingBiomes_);
    switching_ = true;
    joinStarted_ = std::chrono::steady_clock::now();
    lastFromJava_ = joinStarted_;
}

void BedrockSession::forgetEntities() {
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
    pendingSpawns_.clear();
}

void BedrockSession::forgetBackend() {
    const int protocol = connection_.protocolVersion();
    closeWindow(false);
    forgetEntities();
    for (const std::int32_t effect : effects_) {
        wire::io::ByteBuffer gone;
        bedrock::writeMobEffect(gone, kSelfRuntimeId, bedrock::BedrockEffectEvent::Remove, effect, 0, false, 0, false);
        sendBedrock(BedrockPacket::MobEffect, gone);
    }
    for (const auto& [key, entity] : bossBars_) {
        wire::io::ByteBuffer gone;
        bedrock::writeBossEvent(gone, entity, bedrock::BedrockBossAction::Remove, "", 0.0F, protocol);
        sendBedrock(BedrockPacket::BossEvent, gone);
    }
    wire::io::ByteBuffer out;
    if (!sidebar_.empty()) {
        bedrock::writeRemoveObjective(out, sidebar_);
        sendBedrock(BedrockPacket::RemoveObjective, out);
        out.clear();
    }
    bedrock::writeSetTitle(out, bedrock::TitleType::Clear, "", 0, 0, 0);
    sendBedrock(BedrockPacket::SetTitle, out);
    for (const auto weather : {bedrock::LevelEventType::StopRaining, bedrock::LevelEventType::StopThunderstorm}) {
        out.clear();
        bedrock::writeLevelEvent(out, weather, 0.0F, 0.0F, 0.0F, 0);
        sendBedrock(BedrockPacket::LevelEvent, out);
    }
    effects_.clear();
    names_.clear();
    textures_.clear();
    skins_ = std::make_shared<bedrock::SkinCache>();
    bossBars_.clear();
    objectives_.clear();
    sidebar_.clear();
    inventory_ = {};
    sendInventory();
    stateId_ = 0;
    chunks_ = 0;
    spawned_ = false;
    teleported_ = false;
    arriving_ = false;
    respawning_ = false;
    acceptedTeleport_ = -1;
    pendingTeleport_ = -1;
    sneaking_ = false;
    sprinting_ = false;
    pendingMove_.reset();
}

void BedrockSession::hopTo(std::int32_t dimension) {
    const int protocol = connection_.protocolVersion();
    wire::io::ByteBuffer out;
    bedrock::writeChangeDimension(out, static_cast<bedrock::BedrockDimension>(dimension), x_,
                                  y_ + bedrock::kPlayerEyeHeight, z_, true);
    sendBedrock(BedrockPacket::ChangeDimension, out);
    out.clear();
    bedrock::writeStopSound(out, "", true);
    sendBedrock(BedrockPacket::StopSound, out);
    out.clear();
    bedrock::writeDimensionChangeAck(out, kSelfRuntimeId);
    sendBedrock(BedrockPacket::PlayerAction, out);
    const std::int32_t blockX = wire::common::safeInt32(std::floor(x_));
    const std::int32_t blockZ = wire::common::safeInt32(std::floor(z_));
    out.clear();
    bedrock::writeNetworkChunkPublisherUpdate(out, blockX, wire::common::safeInt32(y_), blockZ, kInitialRadius * 16);
    sendBedrock(BedrockPacket::NetworkChunkPublisherUpdate, out);
    for (std::int32_t dx = -kEmptyColumnRadius; dx <= kEmptyColumnRadius; ++dx) {
        for (std::int32_t dz = -kEmptyColumnRadius; dz <= kEmptyColumnRadius; ++dz) {
            out.clear();
            bedrock::writeEmptyLevelChunk(out, (blockX >> 4) + dx, (blockZ >> 4) + dz, biomeSections(dimension),
                                          dimension, protocol);
            sendBedrock(BedrockPacket::LevelChunk, out);
        }
    }
    clientDimension_ = dimension;
}

void BedrockSession::heldSlotChanged(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetHeldSlotPacket::read(in);
    if (!packet || packet->slot < 0 || packet->slot >= 9)
        return;
    heldSlot_ = packet->slot;
    wire::io::ByteBuffer out;
    bedrock::writePlayerHotbar(out, heldSlot_);
    sendBedrock(BedrockPacket::PlayerHotbar, out);
}

} // namespace basalt::proxy::runtime::detail
