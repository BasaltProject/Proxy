#pragma once

#include <basalt/wire/bedrock/BedrockPackets.hpp>
#include <basalt/wire/protocol/play/ChatPackets.hpp>
#include <basalt/wire/protocol/play/CommandPackets.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace basalt::proxy::runtime::detail::bedrockLimits {

namespace bedrock = wire::bedrock;
namespace play = wire::protocol::play;

inline constexpr std::int32_t kInitialRadius = 4;
inline constexpr float kChunksPerTick = 9.0F;
inline constexpr std::uint64_t kOthersBase = 1000;
inline constexpr std::int64_t kTicksPerDay = 24000;
inline constexpr std::int32_t kOverworldMinSection = -4;
inline constexpr std::uint8_t kBabyIndex = 16;
inline constexpr std::uint64_t kSharedMask =
    bedrock::flagBit(bedrock::EntityFlag::OnFire) | bedrock::flagBit(bedrock::EntityFlag::Sneaking) |
    bedrock::flagBit(bedrock::EntityFlag::Sprinting) | bedrock::flagBit(bedrock::EntityFlag::Invisible) |
    bedrock::flagBit(bedrock::EntityFlag::Gliding);
inline constexpr std::int32_t kSwingAction = 1;
inline constexpr std::int32_t kAttackAction = 1;
inline constexpr std::int32_t kTitleKeep = -1;
inline constexpr std::int64_t kBossBase = 0x40000000;
inline constexpr float kMaxHealth = 20.0F;
inline constexpr float kMaxFood = 20.0F;
inline constexpr float kMaxLevel = 24791.0F;
inline constexpr float kWalkSpeed = 0.1F;
inline constexpr std::size_t kCarriedSlots = 36;
inline constexpr std::size_t kCookerSlotCount = 3;
inline constexpr std::int32_t kOutsideWindow = -999;
inline constexpr std::size_t kJavaPlayerSlots = 46;
inline constexpr std::size_t kArmourSlots = 4;
inline constexpr std::size_t kJavaArmourStart = 5;
inline constexpr std::size_t kJavaOffhandSlot = 45;
inline constexpr std::uint32_t kOffhandWindow = 119;
inline constexpr std::uint32_t kArmourWindow = 120;

/// Bedrock numbers the hotbar first; Java puts it last, after the 27 of the main inventory.
[[nodiscard]] inline std::size_t javaSlot(std::size_t bedrockSlot) {
    return bedrockSlot < 9 ? 36 + bedrockSlot : bedrockSlot;
}

inline constexpr std::size_t kBlocksPerSection = 4096;
inline constexpr std::size_t kBiomesPerSection = 64;
inline constexpr std::size_t kMaxCommands = 2000;
inline constexpr std::uint64_t kSelfRuntimeId = 1;
inline constexpr std::uint32_t kPacketIdMask = 0x3ff;
inline constexpr std::string_view kBungeeChannel = "bungeecord:main";
inline constexpr std::string_view kBungeeLegacyChannel = "BungeeCord";
inline constexpr std::size_t kMaxBungeePayload = 32768;
inline constexpr std::size_t kMaxBackendName = 64;
// parity: EntityEvent.HURT.
inline constexpr std::int8_t kJavaHurtEvent = 2;
inline constexpr std::int32_t kPlayerInputId = 0x2b;
inline constexpr std::uint8_t kInputShift = 0x20;
inline constexpr std::uint8_t kInputSprint = 0x40;
inline constexpr std::chrono::milliseconds kPositionInterval{50};
// A client ends each of its ticks with this packet; vanilla refuses a second position before one arrives.
inline constexpr std::int32_t kClientTickEndId = 0x0d;
// Vanilla refreshes its reference position once per tick, so a move sent in the tick of the confirmation looks huge.
inline constexpr std::chrono::milliseconds kSettleTime{150};
// Inputs sent before the client reaches a teleport target are stale and would read as a huge step.
inline constexpr float kArrivalRadius = 2.0F;
inline constexpr std::int32_t kArrivalInputs = 100;
inline constexpr std::chrono::milliseconds kConnectBudget{5000};
inline constexpr std::chrono::seconds kJoinBudget{60};
inline constexpr std::chrono::seconds kSilenceBudget{45};
inline constexpr std::int32_t kChatMessageId = play::ChatMessagePacket::kId;
inline constexpr std::int32_t kChatCommandId = play::ChatCommandPacket::kId;

} // namespace basalt::proxy::runtime::detail::bedrockLimits
