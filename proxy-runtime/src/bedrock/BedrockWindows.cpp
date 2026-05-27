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

void BedrockSession::sendInventory() {
    std::array<bedrock::BedrockItemStack, kCarriedSlots> carried{};
    for (std::size_t slot = 0; slot < kCarriedSlots; ++slot)
        carried[slot] = stackOf(inventory_[javaSlot(slot)]);
    wire::io::ByteBuffer out;
    bedrock::writeInventoryContent(out, 0, carried, bedrock::BedrockContainerSlot::HotbarAndInventory,
                                   connection_.protocolVersion());
    sendBedrock(BedrockPacket::InventoryContent, out);

    std::array<bedrock::BedrockItemStack, kArmourSlots> worn{};
    for (std::size_t slot = 0; slot < kArmourSlots; ++slot)
        worn[slot] = stackOf(inventory_[kJavaArmourStart + slot]);
    out.clear();
    bedrock::writeInventoryContent(out, kArmourWindow, worn, bedrock::BedrockContainerSlot::Armour,
                                   connection_.protocolVersion());
    sendBedrock(BedrockPacket::InventoryContent, out);

    const std::array<bedrock::BedrockItemStack, 1> held{stackOf(inventory_[kJavaOffhandSlot])};
    out.clear();
    bedrock::writeInventoryContent(out, kOffhandWindow, held, bedrock::BedrockContainerSlot::Offhand,
                                   connection_.protocolVersion());
    sendBedrock(BedrockPacket::InventoryContent, out);
}

std::optional<bedrock::BedrockContainerType> BedrockSession::windowType(std::int32_t menu, std::size_t& slots) {
    using Type = bedrock::BedrockContainerType;
    constexpr std::int32_t kGeneric9x1 = 0;
    constexpr std::int32_t kGeneric9x6 = 5;
    constexpr std::int32_t kGeneric3x3 = 6;
    constexpr std::int32_t kBlastFurnace = 10;
    constexpr std::int32_t kFurnace = 14;
    constexpr std::int32_t kHopper = 16;
    constexpr std::int32_t kShulkerBox = 20;
    constexpr std::int32_t kSmoker = 22;
    constexpr std::size_t kRow = 9;
    constexpr std::size_t kCookerSlots = 3;
    if (menu == kShulkerBox) {
        slots = kRow * 3;
        return Type::Container;
    }
    if (menu == kFurnace || menu == kBlastFurnace || menu == kSmoker) {
        slots = kCookerSlots;
        return menu == kFurnace ? Type::Furnace : menu == kBlastFurnace ? Type::BlastFurnace : Type::Smoker;
    }
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
    if (window_ && window_->slots == kCookerSlotCount) {
        if (container == Place::FurnaceIngredient)
            return 0;
        if (container == Place::FurnaceFuel)
            return 1;
        if (container == Place::FurnaceResult)
            return 2;
    }
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
    if (action.type == Type::Drop) {
        if (action.source.container == cursor) {
            click(kOutsideWindow, action.count == 1 ? 1 : 0, Mode::Pickup);
            return true;
        }
        if (source) {
            const std::int32_t held = itemAt(*source) ? itemAt(*source)->count : 0;
            click(*source, action.count >= held ? 1 : 0, Mode::Throw);
            return true;
        }
    }
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

} // namespace basalt::proxy::runtime::detail
