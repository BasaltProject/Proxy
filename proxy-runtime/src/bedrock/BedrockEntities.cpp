#include "../NativeForwarding.hpp"
#include "BedrockSession.hpp"
#include "BedrockSessionLimits.hpp"

#include <basalt/wire/auth/SessionProfile.hpp>
#include <basalt/wire/bedrock/BedrockBiomes.hpp>
#include <basalt/wire/bedrock/BedrockColumn.hpp>
#include <basalt/wire/bedrock/BedrockEffects.hpp>
#include <basalt/wire/bedrock/BedrockEntityIdentifiers.hpp>
#include <basalt/wire/bedrock/BedrockItems.hpp>
#include <basalt/wire/bedrock/BedrockMapConversion.hpp>
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
    bedrock::BedrockItemStack stack{
        .runtimeId = named->palettePlace + 1, .count = item->count, .data = named->data, .blockRuntimeId = 0};
    if (const std::optional<std::int32_t> map = item->mapId(); map)
        stack.mapId = *map;
    if (const std::int32_t explorer = bedrock::bedrockMapExplorerData(javaItem); explorer != 0)
        stack.data = explorer;
    return stack;
}

} // namespace basalt::proxy::runtime::detail
