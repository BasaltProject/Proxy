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

void BedrockSession::sneaking(bool now) {
    if (now == sneaking_)
        return;
    sneaking_ = now;
    wire::io::ByteBuffer out;
    out.writeByte(static_cast<std::uint8_t>((now ? kInputShift : 0) | (sprinting_ ? kInputSprint : 0)));
    sendJava(kPlayerInputId, out);
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

void BedrockSession::relayToBackend(BedrockPacket packet, wire::io::ByteBuffer& body) {
    if (link_ == nullptr || link_->state() != JavaLink::State::kDelegated)
        return;
    wire::io::ByteBuffer out;
    bedrock::writeVarUInt(out, static_cast<std::uint32_t>(packet));
    out.writeBytes(body.readable());
    link_->relay(out.readable());
}

void BedrockSession::relayFromBackend(std::span<const std::uint8_t> packet) {
    lastFromJava_ = std::chrono::steady_clock::now();
    wire::io::ByteBuffer in;
    in.writeBytes(packet);
    const std::uint32_t header = bedrock::readVarUInt(in);
    if (in.failed()) {
        leave("the backend sent a malformed packet");
        return;
    }
    connection_.sendPacket(static_cast<BedrockPacket>(header & kPacketIdMask), in.readable());
    connection_.flush();
}

void BedrockSession::fromBedrock(BedrockPacket packet, wire::io::ByteBuffer& body) {
    if (delegated_) {
        relayToBackend(packet, body);
        return;
    }
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
            if (dx * dx + dy * dy + dz * dz > kArrivalRadius * kArrivalRadius && ++arrivalInputs_ < kArrivalInputs) {
                tickEnded();
                return;
            }
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
        }
        sprinting(input->sprinting);
        sneaking(input->sneaking);
        interact(*input);
        tickEnded();
    } else if (packet == BedrockPacket::MobEquipment) {
        const auto slot = bedrock::readHeldSlot(body, connection_.protocolVersion());
        if (slot && *slot >= 0 && *slot < 9 && *slot != heldSlot_) {
            heldSlot_ = *slot;
            wire::io::ByteBuffer out;
            play::ServerboundSetCarriedItemPacket{static_cast<std::int16_t>(heldSlot_)}.write(out);
            sendJava(play::ServerboundSetCarriedItemPacket::kId, out);
        }
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
