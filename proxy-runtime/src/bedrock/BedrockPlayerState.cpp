#include "../NativeForwarding.hpp"
#include "BedrockSession.hpp"
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

} // namespace

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
            wire::common::safeInt32(strength * static_cast<float>(bedrock::kWeatherStrengthScale)));
        sendBedrock(BedrockPacket::LevelEvent, out);
    };
    if (packet->event == play::GameEventPacket::kChangeGameMode) {
        applyGamemode(wire::common::safeInt32(packet->param));
        wire::io::ByteBuffer mode;
        bedrock::writeSetPlayerGameType(mode, wire::common::safeInt32(packet->param));
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

} // namespace basalt::proxy::runtime::detail
