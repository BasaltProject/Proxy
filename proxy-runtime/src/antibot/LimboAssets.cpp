#include "LimboAssets.hpp"

#include "LimboRegistries.hpp"

#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/io/ByteBuffer.hpp>
#include <basalt/wire/multiversion/ProtocolVersions.hpp>
#include <basalt/wire/multiversion/java/JavaTranslators.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/ProtocolState.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>
#include <basalt/wire/protocol/play/PalettedContainer.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/play/PlayerStatePackets.hpp>
#include <basalt/wire/protocol/play/WorldPackets.hpp>

#include <array>

namespace basalt::proxy::runtime::antibot {
namespace {
namespace protocol = wire::protocol;
namespace play = wire::protocol::play;
namespace configuration = wire::protocol::configuration;
using wire::io::ByteBuffer;
using wire::protocol::ProtocolState;

constexpr std::int32_t kStoneState = 1;
constexpr std::int32_t kSections = 24;
constexpr std::int32_t kMinY = -64;
constexpr std::int32_t kStartConfigurationId = 0x78;
constexpr std::int32_t kViewRadius = 1;

struct Native {
    ProtocolState state;
    std::vector<std::uint8_t> bytes;
};

template <typename Packet>
Native native(ProtocolState state, std::int32_t id, const Packet& packet) {
    ByteBuffer out;
    protocol::varInt::write(out, id);
    packet.write(out);
    const auto bytes = out.readable();
    return {state, {bytes.begin(), bytes.end()}};
}

Native empty(ProtocolState state, std::int32_t id) {
    ByteBuffer out;
    protocol::varInt::write(out, id);
    const auto bytes = out.readable();
    return {state, {bytes.begin(), bytes.end()}};
}

std::vector<std::uint8_t> chunkSections() {
    ByteBuffer out;
    const std::vector<std::int32_t> stone(4096, kStoneState);
    const std::vector<std::int32_t> air(4096, 0);
    const std::vector<std::int32_t> biome(64, 0);
    for (std::int32_t section = 0; section < kSections; ++section) {
        const bool solid = (section * 16 + kMinY) < static_cast<std::int32_t>(kLimboFloorY);
        out.writeShort(solid ? 4096 : 0);
        play::palettedContainer::write(out, solid ? stone : air, play::palettedContainer::kBlockMinIndirectBits,
                                       play::palettedContainer::kBlockMaxIndirectBits, 15);
        play::palettedContainer::write(out, biome, play::palettedContainer::kBiomeMinIndirectBits,
                                       play::palettedContainer::kBiomeMaxIndirectBits, 6);
    }
    const auto bytes = out.readable();
    return {bytes.begin(), bytes.end()};
}

play::Heightmap heightmap(play::HeightmapType type) {
    constexpr std::int64_t kHeight = static_cast<std::int64_t>(kLimboFloorY) - kMinY;
    play::Heightmap result{type, {}};
    constexpr int kBits = 9;
    constexpr int kPerLong = 64 / kBits;
    result.data.assign((256 + kPerLong - 1) / kPerLong, 0);
    for (std::size_t i = 0; i < 256; ++i)
        result.data[i / kPerLong] |= kHeight << ((i % kPerLong) * kBits);
    return result;
}

std::vector<Native> configIntroPackets() {
    std::vector<Native> result;
    result.push_back(native(ProtocolState::Configuration, configuration::BrandPacket::kId,
                            configuration::BrandPacket{"Basalt Proxy"}));
    result.push_back(native(ProtocolState::Configuration, configuration::UpdateEnabledFeaturesPacket::kId,
                            configuration::UpdateEnabledFeaturesPacket{{"minecraft:vanilla"}}));
    result.push_back(native(ProtocolState::Configuration, configuration::SelectKnownPacksPacket::kClientboundId,
                            configuration::SelectKnownPacksPacket{{configuration::KnownPack{
                                "minecraft", "core", std::string{buildInfo::kMinecraftVersion}}}}));
    return result;
}

std::vector<Native> configRestPackets() {
    std::vector<Native> result;
    for (const auto& registry : limboRegistries()) {
        configuration::RegistryDataPacket packet{std::string{registry.registry}, {}};
        packet.entries.reserve(registry.entries.size());
        for (const auto entry : registry.entries)
            packet.entries.push_back(configuration::RegistryEntry{"minecraft:" + std::string{entry}, std::nullopt});
        result.push_back(native(ProtocolState::Configuration, configuration::RegistryDataPacket::kId, packet));
    }
    for (const std::string_view empty :
         {"minecraft:trim_pattern", "minecraft:sulfur_cube_archetype", "minecraft:enchantment",
          "minecraft:test_environment", "minecraft:test_instance", "minecraft:dialog", "minecraft:timeline"})
        result.push_back(native(ProtocolState::Configuration, configuration::RegistryDataPacket::kId,
                                configuration::RegistryDataPacket{std::string{empty}, {}}));
    result.push_back(native(ProtocolState::Configuration, configuration::TagsPacket::kId, configuration::TagsPacket{}));
    result.push_back(empty(ProtocolState::Configuration, configuration::kFinishConfigurationId));
    return result;
}

std::vector<Native> playIntroPackets(std::int32_t dropHeight) {
    std::vector<Native> result;
    play::LoginPlayPacket login;
    login.entityId = 1;
    login.worldNames = {"minecraft:overworld"};
    login.maxPlayers = 1;
    login.viewDistance = 2;
    login.simulationDistance = 2;
    login.enableRespawnScreen = false;
    login.spawnInfo.dimension = 0;
    login.spawnInfo.dimensionName = "minecraft:overworld";
    login.spawnInfo.gamemode = play::Gamemode::Adventure;
    login.spawnInfo.isFlat = true;
    result.push_back(native(ProtocolState::Play, play::LoginPlayPacket::kId, login));
    result.push_back(native(ProtocolState::Play, play::ClientboundPlayerAbilitiesPacket::kId,
                            play::ClientboundPlayerAbilitiesPacket{.invulnerable = true}));
    result.push_back(native(ProtocolState::Play, play::SpawnPositionPacket::kId,
                            play::SpawnPositionPacket{
                                {"minecraft:overworld", {8, static_cast<std::int32_t>(kLimboFloorY), 8}}, 0.0F, 0.0F}));
    result.push_back(native(ProtocolState::Play, play::GameEventPacket::kId,
                            play::GameEventPacket{play::GameEventPacket::kLevelChunksLoadStart, 0.0F}));
    result.push_back(
        native(ProtocolState::Play, play::SetChunkCacheCenterPacket::kId, play::SetChunkCacheCenterPacket{0, 0}));
    result.push_back(empty(ProtocolState::Play, play::kChunkBatchStartId));
    const auto sections = chunkSections();
    const auto surface = heightmap(play::HeightmapType::WorldSurface);
    const auto motion = heightmap(play::HeightmapType::MotionBlocking);
    std::int32_t chunks = 0;
    for (std::int32_t x = -kViewRadius; x <= kViewRadius; ++x)
        for (std::int32_t z = -kViewRadius; z <= kViewRadius; ++z, ++chunks) {
            play::MapChunkPacket chunk;
            chunk.x = x;
            chunk.z = z;
            chunk.heightmaps = {surface, motion};
            chunk.chunkData = sections;
            result.push_back(native(ProtocolState::Play, play::MapChunkPacket::kId, chunk));
        }
    result.push_back(
        native(ProtocolState::Play, play::ChunkBatchFinishedPacket::kId, play::ChunkBatchFinishedPacket{chunks}));
    play::PlayerPositionPacket position;
    position.teleportId = kLimboTeleportId;
    position.x = kLimboSpawnX;
    position.y = kLimboFloorY + dropHeight;
    position.z = kLimboSpawnZ;
    result.push_back(native(ProtocolState::Play, play::PlayerPositionPacket::kId, position));
    return result;
}

void frame(std::vector<std::uint8_t>& out, std::int32_t threshold, std::span<const std::uint8_t> packet) {
    ByteBuffer framed;
    if (threshold >= 0)
        (void)protocol::packetFraming::writeCompressed(framed, packet, threshold);
    else
        (void)protocol::packetFraming::writeUncompressed(framed, packet);
    const auto bytes = framed.readable();
    out.insert(out.end(), bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> lay(std::span<const Native> packets, const wire::multiversion::java::JavaChain* chain,
                              std::int32_t threshold) {
    std::vector<std::uint8_t> out;
    for (const auto& packet : packets) {
        if (!chain) {
            frame(out, threshold, packet.bytes);
            continue;
        }
        ByteBuffer view;
        view.writeBytes(packet.bytes);
        const auto id = protocol::varInt::read(view);
        chain->chain().clientbound(packet.state, id, view.readable(),
                                   [&](std::int32_t translatedId, std::span<const std::uint8_t> body) {
                                       ByteBuffer whole;
                                       protocol::varInt::write(whole, translatedId);
                                       whole.writeBytes(body);
                                       frame(out, threshold, whole.readable());
                                   });
    }
    return out;
}
} // namespace

bool LimboAssets::serves(std::int32_t protocol) {
    return protocol == buildInfo::kProtocolVersion ||
           (protocol >= wire::multiversion::protocolVersions::kV1_20_5 &&
            wire::multiversion::java::javaTranslators::supported().contains(protocol));
}

std::shared_ptr<const LimboPackets> LimboAssets::forProtocol(std::int32_t protocol, std::int32_t dropHeight) {
    if (!serves(protocol))
        return nullptr;
    const std::lock_guard lock{mutex_};
    const auto key = std::pair{protocol, dropHeight};
    if (const auto found = cache_.find(key); found != cache_.end())
        return found->second;
    std::unique_ptr<wire::multiversion::java::JavaChain> chain;
    if (protocol != buildInfo::kProtocolVersion) {
        chain = wire::multiversion::java::javaTranslators::chainFor(protocol);
        if (!chain)
            return nullptr;
    }
    auto built = std::make_shared<LimboPackets>();
    const auto intro = configIntroPackets();
    const auto rest = configRestPackets();
    const auto play = playIntroPackets(dropHeight);
    const std::array<Native, 1> start{empty(ProtocolState::Play, kStartConfigurationId)};
    built->configIntro = lay(intro, chain.get(), threshold_);
    built->configRest = lay(rest, chain.get(), threshold_);
    built->playIntro = lay(play, chain.get(), threshold_);
    built->startConfiguration = lay(start, chain.get(), threshold_);
    cache_.emplace(key, built);
    return built;
}

} // namespace basalt::proxy::runtime::antibot
