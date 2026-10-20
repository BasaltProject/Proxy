#include "../NativeForwarding.hpp"
#include "BedrockSession.hpp"
#include "BedrockSessionLimits.hpp"

#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/bedrock/BedrockBiomes.hpp>
#include <basalt/wire/bedrock/BedrockColumn.hpp>
#include <basalt/wire/bedrock/BedrockEffects.hpp>
#include <basalt/wire/bedrock/BedrockEntityIdentifiers.hpp>
#include <basalt/wire/bedrock/BedrockItems.hpp>
#include <basalt/wire/bedrock/BedrockJavaBlockEntities.hpp>
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
        },
        [&](wire::io::ByteBuffer& tags) {
            const auto stateAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
                const std::int32_t section = ((y - (minSection_ * 16)) >> 4);
                if (section < 0 || static_cast<std::size_t>(section) >= blocks.size())
                    return 0;
                const std::size_t at = (static_cast<std::size_t>(y & 15) << 8) | (static_cast<std::size_t>(z) << 4) |
                                       static_cast<std::size_t>(x);
                return blocks[static_cast<std::size_t>(section)][at];
            };
            for (std::size_t section = 0; section < blocks.size(); ++section) {
                for (std::size_t at = 0; at < kBlocksPerSection; ++at) {
                    const auto colour = bedrock::javaBedColour(blocks[section][at]);
                    if (!colour)
                        continue;
                    const auto y = static_cast<std::int32_t>(((minSection_ + static_cast<std::int32_t>(section)) * 16) +
                                                             static_cast<std::int32_t>(at >> 8));
                    bedrock::writeBedTag(tags, (packet->x * 16) + static_cast<std::int32_t>(at & 15), y,
                                         (packet->z * 16) + static_cast<std::int32_t>((at >> 4) & 15), *colour);
                }
            }
            for (const auto& entity : packet->blockEntities) {
                if (!entity.tag)
                    continue;
                const std::int32_t localX = (entity.packedXZ >> 4) & 0xF;
                const std::int32_t localZ = entity.packedXZ & 0xF;
                bedrock::writeJavaBlockEntity(tags, (packet->x * 16) + localX, entity.y, (packet->z * 16) + localZ,
                                              entity.typeId, *entity.tag, stateAt(localX, entity.y, localZ));
            }
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
    bedrock::writeNetworkChunkPublisherUpdate(out, wire::common::safeInt32(x_), wire::common::safeInt32(y_),
                                              wire::common::safeInt32(z_), kInitialRadius * 16);
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

} // namespace basalt::proxy::runtime::detail
