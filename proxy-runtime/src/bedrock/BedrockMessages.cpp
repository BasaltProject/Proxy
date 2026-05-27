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

void BedrockSession::sound(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SoundPacket::read(in);
    if (!packet)
        return;
    const std::string_view named = bedrock::bedrockSoundName(packet->sound);
    if (named.empty())
        return;
    wire::io::ByteBuffer out;
    bedrock::writePlaySound(out, named, packet->x, packet->y, packet->z, packet->volume, packet->pitch,
                            connection_.protocolVersion());
    sendBedrock(BedrockPacket::PlaySound, out);
}

void BedrockSession::pluginMessage(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto message = play::CustomPayloadPacket::read(in);
    if (!message || !backend_.bungeeMessages || !commands_.connect ||
        (message->channel != kBungeeChannel && message->channel != kBungeeLegacyChannel) ||
        message->data.size() > kMaxBungeePayload)
        return;
    wire::io::ByteBuffer data;
    data.writeBytes(message->data);
    const auto text = [&data]() -> std::string {
        const auto length = static_cast<std::uint16_t>(data.readShort());
        const auto bytes = data.readSpan(length);
        return data.failed() ? std::string{} : std::string{bytes.begin(), bytes.end()};
    };
    const std::string subchannel = text();
    if (subchannel == "Connect") {
        const std::string target = text();
        if (!target.empty() && target.size() <= kMaxBackendName)
            static_cast<void>(commands_.connect(javaName_, target));
    } else if (subchannel == "GetServer") {
        bungeeAnswer(message->channel, subchannel, backend_.name);
    } else if (subchannel == "GetServers" && commands_.servers) {
        std::string names;
        for (const std::string& name : commands_.servers()) {
            if (names.size() + name.size() + 2 > kMaxBungeePayload)
                break;
            names += names.empty() ? name : ", " + name;
        }
        bungeeAnswer(message->channel, subchannel, names);
    } else if (subchannel == "UUID") {
        std::string plain = javaUuid_;
        std::erase(plain, '-');
        bungeeAnswer(message->channel, subchannel, plain);
    }
}

void BedrockSession::bungeeAnswer(const std::string& channel, std::string_view subchannel, std::string_view value) {
    wire::io::ByteBuffer data;
    for (const std::string_view part : {subchannel, value}) {
        data.writeShort(static_cast<std::int16_t>(part.size()));
        data.writeBytes(std::span{reinterpret_cast<const std::uint8_t*>(part.data()), part.size()});
    }
    play::CustomPayloadPacket reply;
    reply.channel = channel;
    reply.data.assign(data.readable().begin(), data.readable().end());
    wire::io::ByteBuffer out;
    reply.write(out);
    sendJava(play::CustomPayloadPacket::kServerboundId, out);
}

void BedrockSession::hurt(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::HurtAnimationPacket::read(in);
    if (!packet ||
        (packet->entityId != selfEntity_ && !mobs_.contains(packet->entityId) && !others_.contains(packet->entityId)))
        return;
    const std::uint64_t entity = packet->entityId == selfEntity_ ? kSelfRuntimeId : runtimeId(packet->entityId);
    wire::io::ByteBuffer out;
    bedrock::writeEntityEvent(out, entity, bedrock::BedrockEntityEvent::Hurt, 0, connection_.protocolVersion());
    sendBedrock(BedrockPacket::EntityEvent, out);
}

void BedrockSession::entityEvent(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::EntityEventPacket::read(in);
    if (!packet)
        return;
    const bool self = packet->entityId == selfEntity_;
    const bool known = self || mobs_.contains(packet->entityId) || others_.contains(packet->entityId);
    const bool hurt = packet->eventId == kJavaHurtEvent;
    const bool died = packet->eventId == play::EntityEventPacket::kLivingDeath && !self;
    if (!known || (!hurt && !died))
        return;
    wire::io::ByteBuffer out;
    bedrock::writeEntityEvent(out, self ? kSelfRuntimeId : runtimeId(packet->entityId),
                              hurt ? bedrock::BedrockEntityEvent::Hurt : bedrock::BedrockEntityEvent::Death, 0,
                              connection_.protocolVersion());
    sendBedrock(BedrockPacket::EntityEvent, out);
}

void BedrockSession::effectGiven(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::UpdateMobEffectPacket::read(in);
    if (!packet || packet->entityId != selfEntity_)
        return;
    const std::int32_t effect = bedrock::bedrockEffectId(packet->effect);
    if (effect == 0)
        return;
    const auto event =
        effects_.insert(effect).second ? bedrock::BedrockEffectEvent::Add : bedrock::BedrockEffectEvent::Modify;
    wire::io::ByteBuffer out;
    bedrock::writeMobEffect(out, kSelfRuntimeId, event, effect, packet->amplifier, packet->visible,
                            packet->durationTicks, packet->ambient);
    sendBedrock(BedrockPacket::MobEffect, out);
}

void BedrockSession::effectRemoved(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::RemoveMobEffectPacket::read(in);
    if (!packet || packet->entityId != selfEntity_)
        return;
    const std::int32_t effect = bedrock::bedrockEffectId(packet->effect);
    if (effect == 0 || effects_.erase(effect) == 0)
        return;
    wire::io::ByteBuffer out;
    bedrock::writeMobEffect(out, kSelfRuntimeId, bedrock::BedrockEffectEvent::Remove, effect, 0, false, 0, false);
    sendBedrock(BedrockPacket::MobEffect, out);
}

void BedrockSession::commandList(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto tree = play::CommandsPacket::read(in);
    if (!tree || tree->rootIndex < 0 || static_cast<std::size_t>(tree->rootIndex) >= tree->nodes.size())
        return;
    std::vector<bedrock::CommandInfo> commands;
    for (const std::int32_t child : tree->nodes[static_cast<std::size_t>(tree->rootIndex)].children) {
        if (child < 0 || static_cast<std::size_t>(child) >= tree->nodes.size() || commands.size() >= kMaxCommands)
            continue;
        const auto& node = tree->nodes[static_cast<std::size_t>(child)];
        if (node.kind == play::CommandNodeData::Kind::Literal && node.name && !node.name->empty())
            commands.push_back({*node.name, {}});
    }
    if (commands_.specs) {
        for (const auto& spec : commands_.specs()) {
            if (spec.literals.empty() || commands.size() >= kMaxCommands)
                continue;
            const auto known = [&](const bedrock::CommandInfo& info) { return info.name == spec.literals.front(); };
            if (std::ranges::none_of(commands, known))
                commands.push_back({spec.literals.front(), {}});
        }
    }
    wire::io::ByteBuffer out;
    bedrock::writeAvailableCommands(out, commands);
    sendBedrock(BedrockPacket::AvailableCommands, out);
}

void BedrockSession::kicked(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::PlayDisconnectPacket::read(in);
    std::string reason = packet ? wire::text::toLegacyText(resolved(packet->reason)) : std::string{};
    if (events_ && packet)
        events_({runtime::PlayerEventKind::kKick, javaName_, backend_.name, javaUuid_,
                 wire::text::toPlainText(resolved(packet->reason))});
    leave(reason.empty() ? "The server closed the connection." : reason);
}

void BedrockSession::chat(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SystemChatPacket::read(in);
    if (!packet)
        return;
    wire::io::ByteBuffer out;
    const std::string shown = wire::text::toLegacyText(resolved(packet->content));
    if (packet->isActionBar) {
        bedrock::writeSetTitle(out, bedrock::TitleType::ActionBar, shown, kTitleKeep, kTitleKeep, kTitleKeep);
        sendBedrock(BedrockPacket::SetTitle, out);
        return;
    }
    bedrock::writeText(out, bedrock::TextType::Raw, "", shown, "");
    sendBedrock(BedrockPacket::Text, out);
}

void BedrockSession::playerChat(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::PlayerChatPacket::read(in);
    if (!packet)
        return;
    const auto known = names_.find(packet->sender.toString());
    const std::string name =
        known != names_.end() ? known->second : wire::text::toPlainText(resolved(packet->chatType.name));
    const std::string message =
        packet->unsignedContent ? wire::text::toLegacyText(resolved(*packet->unsignedContent)) : packet->content;
    wire::io::ByteBuffer out;
    bedrock::writeText(out, bedrock::TextType::Chat, name, message, "");
    sendBedrock(BedrockPacket::Text, out);
}

void BedrockSession::title(bedrock::TitleType type, std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::TitleTextPacket::read(in);
    if (!packet)
        return;
    wire::io::ByteBuffer out;
    bedrock::writeSetTitle(out, type, wire::text::toLegacyText(resolved(packet->text)), kTitleKeep, kTitleKeep,
                           kTitleKeep);
    sendBedrock(BedrockPacket::SetTitle, out);
}

void BedrockSession::titleTimes(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetTitlesAnimationPacket::read(in);
    if (!packet)
        return;
    wire::io::ByteBuffer out;
    bedrock::writeSetTitle(out, bedrock::TitleType::Times, "", packet->fadeIn, packet->stay, packet->fadeOut);
    sendBedrock(BedrockPacket::SetTitle, out);
}

void BedrockSession::titlesCleared(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::ClearTitlesPacket::read(in);
    if (!packet)
        return;
    wire::io::ByteBuffer out;
    bedrock::writeSetTitle(out, packet->resetTimes ? bedrock::TitleType::Reset : bedrock::TitleType::Clear, "", 0, 0,
                           0);
    sendBedrock(BedrockPacket::SetTitle, out);
}

wire::text::Component BedrockSession::resolved(const wire::text::Component& component) const {
    return language_ != nullptr ? wire::text::resolveComponent(component, *language_) : component;
}

std::string BedrockSession::sidebarTitle() const {
    const auto found = objectives_.find(sidebar_);
    return found == objectives_.end() ? sidebar_ : found->second.title;
}

void BedrockSession::drawSidebar() {
    if (sidebar_.empty())
        return;
    wire::io::ByteBuffer out;
    bedrock::writeSetDisplayObjective(out, "sidebar", sidebar_, sidebarTitle());
    sendBedrock(BedrockPacket::SetDisplayObjective, out);
    std::vector<bedrock::BedrockScoreLine> lines;
    for (const auto& [owner, line] : objectives_[sidebar_].lines)
        lines.push_back({line.id, line.text, line.score});
    if (lines.empty())
        return;
    out.clear();
    bedrock::writeSetScore(out, sidebar_, lines, connection_.protocolVersion());
    sendBedrock(BedrockPacket::SetScore, out);
}

void BedrockSession::objective(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetObjectivePacket::read(in);
    if (!packet)
        return;
    if (packet->method == play::SetObjectivePacket::kMethodRemove) {
        if (packet->objectiveName == sidebar_) {
            wire::io::ByteBuffer out;
            bedrock::writeRemoveObjective(out, sidebar_);
            sendBedrock(BedrockPacket::RemoveObjective, out);
            sidebar_.clear();
        }
        objectives_.erase(packet->objectiveName);
        return;
    }
    Objective& stored = objectives_[packet->objectiveName];
    if (packet->displayName)
        stored.title = wire::text::toLegacyText(resolved(*packet->displayName));
    if (packet->objectiveName == sidebar_)
        drawSidebar();
}

void BedrockSession::displayObjective(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetDisplayObjectivePacket::read(in);
    if (!packet || packet->slot != play::DisplaySlot::Sidebar)
        return;
    if (!sidebar_.empty() && sidebar_ != packet->objectiveName) {
        wire::io::ByteBuffer out;
        bedrock::writeRemoveObjective(out, sidebar_);
        sendBedrock(BedrockPacket::RemoveObjective, out);
    }
    sidebar_ = packet->objectiveName;
    drawSidebar();
}

void BedrockSession::scoreSet(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::SetScorePacket::read(in);
    if (!packet)
        return;
    Objective& stored = objectives_[packet->objectiveName];
    auto [line, added] = stored.lines.try_emplace(packet->owner);
    if (added)
        line->second.id = ++nextLine_;
    line->second.text = packet->display ? wire::text::toLegacyText(resolved(*packet->display)) : packet->owner;
    line->second.score = packet->score;
    if (packet->objectiveName != sidebar_)
        return;
    const bedrock::BedrockScoreLine drawn{line->second.id, line->second.text, line->second.score};
    wire::io::ByteBuffer out;
    bedrock::writeSetScore(out, sidebar_, std::span{&drawn, 1}, connection_.protocolVersion());
    sendBedrock(BedrockPacket::SetScore, out);
}

void BedrockSession::scoreReset(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::ResetScorePacket::read(in);
    if (!packet)
        return;
    for (auto& [name, stored] : objectives_) {
        if (packet->objectiveName && *packet->objectiveName != name)
            continue;
        const auto line = stored.lines.find(packet->owner);
        if (line == stored.lines.end())
            continue;
        if (name == sidebar_) {
            const std::int64_t id = line->second.id;
            wire::io::ByteBuffer out;
            bedrock::writeRemoveScores(out, name, std::span{&id, 1}, connection_.protocolVersion());
            sendBedrock(BedrockPacket::SetScore, out);
        }
        stored.lines.erase(line);
    }
}

void BedrockSession::bossEvent(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    const auto packet = play::BossEventPacket::read(in);
    if (!packet)
        return;
    const std::string key = packet->id.toString();
    wire::io::ByteBuffer out;
    if (const auto* add = std::get_if<play::BossEventPacket::Add>(&packet->operation)) {
        const std::int64_t entity = kBossBase + static_cast<std::int64_t>(bossBars_.size()) + 1;
        bossBars_[key] = entity;
        bedrock::writeBossEvent(out, entity, bedrock::BedrockBossAction::Create,
                                wire::text::toLegacyText(resolved(add->name)), add->progress,
                                connection_.protocolVersion());
        sendBedrock(BedrockPacket::BossEvent, out);
        return;
    }
    const auto known = bossBars_.find(key);
    if (known == bossBars_.end())
        return;
    if (std::holds_alternative<play::BossEventPacket::Remove>(packet->operation)) {
        bedrock::writeBossEvent(out, known->second, bedrock::BedrockBossAction::Remove, "", 0.0F,
                                connection_.protocolVersion());
        bossBars_.erase(known);
    } else if (const auto* progress = std::get_if<play::BossEventPacket::UpdateProgress>(&packet->operation)) {
        bedrock::writeBossEvent(out, known->second, bedrock::BedrockBossAction::UpdatePercentage, "",
                                progress->progress, connection_.protocolVersion());
    } else if (const auto* name = std::get_if<play::BossEventPacket::UpdateName>(&packet->operation)) {
        bedrock::writeBossEvent(out, known->second, bedrock::BedrockBossAction::UpdateName,
                                wire::text::toLegacyText(resolved(name->name)), 0.0F, connection_.protocolVersion());
    } else {
        return;
    }
    sendBedrock(BedrockPacket::BossEvent, out);
}

} // namespace basalt::proxy::runtime::detail
