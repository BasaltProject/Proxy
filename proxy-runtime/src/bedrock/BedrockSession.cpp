#include "BedrockSession.hpp"

#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/bedrock/BedrockBiomes.hpp>
#include <basalt/wire/bedrock/BedrockItems.hpp>
#include <basalt/wire/bedrock/BedrockPackets.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/play/CommandPackets.hpp>
#include <basalt/wire/protocol/play/PalettedContainer.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/play/WorldPackets.hpp>
#include <basalt/wire/text/Serializers.hpp>

#include <array>
#include <chrono>
#include <cmath>

namespace basalt::proxy::runtime::detail {
namespace {

namespace bedrock = wire::bedrock;
namespace play = wire::protocol::play;
using bedrock::BedrockPacket;

constexpr std::int32_t kInitialRadius = 4;
constexpr float kChunksPerTick = 9.0F;
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
                               std::string address)
    : connection_(connection), backend_(std::move(backend)), address_(std::move(address)) {}

bool BedrockSession::begin() {
    const bedrock::LoginRequest& login = connection_.login();
    wire::auth::SessionProfile profile;
    profile.name = javaName(login.name);
    profile.id = wire::common::Uuid::offlinePlayer(profile.name);
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
        if (!input || !spawned_)
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
    } else if (packet == BedrockPacket::Text) {
        const auto text = bedrock::readText(body);
        if (!text)
            return;
        wire::io::ByteBuffer out;
        play::ChatMessagePacket message;
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
