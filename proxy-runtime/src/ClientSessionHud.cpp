#include "ClientSession.hpp"

#include <basalt/wire/nbt/NbtTag.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>
#include <basalt/wire/protocol/play/BorderPackets.hpp>
#include <basalt/wire/protocol/play/HudPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/play/UiPackets.hpp>
#include <basalt/wire/text/ComponentNbt.hpp>

#include <algorithm>

namespace basalt::proxy::runtime::detail {
namespace {

namespace play = wire::protocol::play;

constexpr std::int32_t kPlayCookieRequestId = 0x15;
constexpr std::int32_t kPlayCookieStoreId = 0x7a;
constexpr std::size_t kMaxPluginCookieRequests = 8;
constexpr std::size_t kMaxPluginTabEntries = 80;
constexpr std::int32_t kConfigClearDialogId = 0x12;
constexpr std::int32_t kConfigShowDialogId = 0x13;
constexpr std::int32_t kPlayClearDialogId = 0x8e;
constexpr std::int32_t kPlayShowDialogId = 0x8f;

template <typename Packet>
void frame(wire::io::ByteBuffer& out, std::int32_t id, const Packet& packet) {
    out.clear();
    wire::protocol::varInt::write(out, id);
    packet.write(out);
}

play::BossBarColor colorOf(std::int32_t value) { return static_cast<play::BossBarColor>(std::clamp(value, 0, 6)); }

play::BossBarOverlay overlayOf(std::int32_t value) {
    return static_cast<play::BossBarOverlay>(std::clamp(value, 0, 4));
}

wire::common::Uuid barId(const std::string& name) { return wire::common::Uuid::offlinePlayer("bossbar:" + name); }

} // namespace

bool ClientSession::applyDialog(const HudDialog& dialog) {
    const bool configuring = state_ == State::kConfiguration;
    if (closing_ || clientProtocol_ != 0 || legacyRaw_ || (!configuring && !inPlay()))
        return false;
    wire::io::ByteBuffer out;
    wire::protocol::varInt::write(out, configuring ? kConfigClearDialogId : kPlayClearDialogId);
    if (dialog.kind == HudDialog::Kind::kClear)
        return queuePacket(out.readable());
    const auto button = [](const HudDialog::Button& data) {
        wire::nbt::NbtCompound result;
        result.set("label", wire::text::componentNbt::serialize(data.label));
        if (!data.command.empty()) {
            wire::nbt::NbtCompound action;
            action.putString("type", "minecraft:run_command");
            action.putString("command", data.command);
            result.set("action", wire::nbt::NbtTag::ofCompound(std::move(action)));
        }
        return wire::nbt::NbtTag::ofCompound(std::move(result));
    };
    wire::nbt::NbtCompound root;
    const bool confirmation = dialog.kind == HudDialog::Kind::kConfirmation;
    const bool multi = dialog.kind == HudDialog::Kind::kMultiAction;
    root.putString("type",
                   confirmation ? "minecraft:confirmation" : (multi ? "minecraft:multi_action" : "minecraft:notice"));
    root.set("title", wire::text::componentNbt::serialize(dialog.title));
    root.putBoolean("can_close_with_escape", dialog.canCloseWithEscape);
    std::vector<wire::nbt::NbtTag> body;
    for (const auto& line : dialog.body) {
        wire::nbt::NbtCompound message;
        message.putString("type", "minecraft:plain_message");
        message.set("contents", wire::text::componentNbt::serialize(line));
        body.push_back(wire::nbt::NbtTag::ofCompound(std::move(message)));
    }
    root.set("body", wire::nbt::NbtTag::ofList(wire::nbt::NbtList{wire::nbt::NbtType::Compound, std::move(body)}));
    if (confirmation) {
        if (dialog.buttons.size() != 2)
            return false;
        root.set("yes", button(dialog.buttons[0]));
        root.set("no", button(dialog.buttons[1]));
    } else if (multi) {
        if (dialog.buttons.empty())
            return false;
        std::vector<wire::nbt::NbtTag> actions;
        for (const auto& entry : dialog.buttons)
            actions.push_back(button(entry));
        root.set("actions",
                 wire::nbt::NbtTag::ofList(wire::nbt::NbtList{wire::nbt::NbtType::Compound, std::move(actions)}));
    } else if (!dialog.buttons.empty()) {
        root.set("action", button(dialog.buttons[0]));
    }
    out.clear();
    wire::protocol::varInt::write(out, configuring ? kConfigShowDialogId : kPlayShowDialogId);
    if (!configuring)
        wire::protocol::varInt::write(out, 0);
    wire::protocol::writeAnonymousNbt(out, wire::nbt::NbtTag::ofCompound(std::move(root)));
    return queuePacket(out.readable());
}

bool ClientSession::applyHud(const PlayerHud& action) {
    if (const auto* dialog = std::get_if<HudDialog>(&action))
        return applyDialog(*dialog);
    if (!inPlay() || closing_ || clientProtocol_ != 0 || legacyRaw_)
        return false;
    wire::io::ByteBuffer out;
    const auto send = [&] { return queuePacket(out.readable()); };
    if (const auto* title = std::get_if<HudTitle>(&action)) {
        if (title->clear) {
            frame(out, play::ClearTitlesPacket::kId, play::ClearTitlesPacket{title->reset});
            if (!send())
                return false;
        }
        if (title->times) {
            frame(out, play::SetTitlesAnimationPacket::kId,
                  play::SetTitlesAnimationPacket{(*title->times)[0], (*title->times)[1], (*title->times)[2]});
            if (!send())
                return false;
        }
        if (title->subtitle) {
            frame(out, play::kSetSubtitleTextId, play::TitleTextPacket{*title->subtitle});
            if (!send())
                return false;
        }
        if (title->title) {
            frame(out, play::kSetTitleTextId, play::TitleTextPacket{*title->title});
            if (!send())
                return false;
        }
        if (title->actionBar) {
            frame(out, play::kSetActionBarTextId, play::TitleTextPacket{*title->actionBar});
            if (!send())
                return false;
        }
        return true;
    }
    if (const auto* tab = std::get_if<HudTabList>(&action)) {
        if (tab->header)
            tabHeader_ = tab->header;
        if (tab->footer)
            tabFooter_ = tab->footer;
        frame(out, play::TabListPacket::kId,
              play::TabListPacket{tabHeader_.value_or(wire::text::Component::empty()),
                                  tabFooter_.value_or(wire::text::Component::empty())});
        return send();
    }
    if (const auto* entry = std::get_if<HudTabEntry>(&action)) {
        const auto uuid = wire::common::Uuid::offlinePlayer("tabentry:" + entry->id);
        if (entry->remove) {
            if (pluginTabEntries_.erase(entry->id) == 0)
                return true;
            frame(out, play::PlayerInfoRemovePacket::kId, play::PlayerInfoRemovePacket{{uuid}});
            return send();
        }
        if (pluginTabEntries_.size() >= kMaxPluginTabEntries && !pluginTabEntries_.contains(entry->id))
            return false;
        play::PlayerInfoUpdatePacket packet;
        packet.actions = play::PlayerInfoUpdatePacket::kAddPlayer | play::PlayerInfoUpdatePacket::kUpdateListed |
                         play::PlayerInfoUpdatePacket::kUpdateLatency |
                         play::PlayerInfoUpdatePacket::kUpdateDisplayName;
        play::PlayerInfoEntry row;
        row.uuid = uuid;
        row.name = entry->name.substr(0, 16);
        row.listed = true;
        row.latencyMillis = entry->latencyMillis;
        row.displayName = entry->text;
        packet.entries.push_back(std::move(row));
        pluginTabEntries_.insert(entry->id);
        frame(out, play::PlayerInfoUpdatePacket::kId, packet);
        return send();
    }
    if (const auto* sidebar = std::get_if<HudSidebar>(&action)) {
        constexpr std::string_view kObjective = "basalt_sidebar";
        if (sidebar->remove) {
            if (!sidebarShown_)
                return true;
            play::SetObjectivePacket gone;
            gone.objectiveName = std::string{kObjective};
            gone.method = play::SetObjectivePacket::kMethodRemove;
            frame(out, play::SetObjectivePacket::kId, gone);
            sidebarShown_ = false;
            sidebarLines_ = 0;
            return send();
        }
        play::SetObjectivePacket objective;
        objective.objectiveName = std::string{kObjective};
        objective.method =
            sidebarShown_ ? play::SetObjectivePacket::kMethodChange : play::SetObjectivePacket::kMethodAdd;
        objective.displayName = sidebar->title;
        frame(out, play::SetObjectivePacket::kId, objective);
        if (!send())
            return false;
        if (!sidebarShown_) {
            frame(out, play::SetDisplayObjectivePacket::kId,
                  play::SetDisplayObjectivePacket{play::DisplaySlot::Sidebar, std::string{kObjective}});
            if (!send())
                return false;
        }
        const std::size_t count = std::min<std::size_t>(sidebar->lines.size(), 15);
        for (std::size_t line = 0; line < count; ++line) {
            play::SetScorePacket score;
            score.owner = "basalt_line_" + std::to_string(line);
            score.objectiveName = std::string{kObjective};
            score.score = static_cast<std::int32_t>(count - line);
            score.display = sidebar->lines[line];
            score.numberFormat = play::NumberFormat{play::NumberFormat::Blank{}};
            frame(out, play::SetScorePacket::kId, score);
            if (!send())
                return false;
        }
        for (std::size_t line = count; line < sidebarLines_; ++line) {
            frame(out, play::ResetScorePacket::kId,
                  play::ResetScorePacket{"basalt_line_" + std::to_string(line), std::string{kObjective}});
            if (!send())
                return false;
        }
        sidebarShown_ = true;
        sidebarLines_ = count;
        return true;
    }
    if (const auto* bar = std::get_if<HudBossBar>(&action)) {
        play::BossEventPacket packet;
        packet.id = barId(bar->id);
        switch (bar->op) {
        case HudBossBar::Op::kAdd:
            packet.operation = play::BossEventPacket::Add{
                bar->name, bar->progress, colorOf(bar->color), overlayOf(bar->overlay), false, false, false};
            break;
        case HudBossBar::Op::kRemove:
            packet.operation = play::BossEventPacket::Remove{};
            break;
        case HudBossBar::Op::kProgress:
            packet.operation = play::BossEventPacket::UpdateProgress{bar->progress};
            break;
        case HudBossBar::Op::kName:
            packet.operation = play::BossEventPacket::UpdateName{bar->name};
            break;
        case HudBossBar::Op::kStyle:
            packet.operation = play::BossEventPacket::UpdateStyle{colorOf(bar->color), overlayOf(bar->overlay)};
            break;
        }
        frame(out, play::BossEventPacket::kId, packet);
        return send();
    }
    if (const auto* transfer = std::get_if<HudTransfer>(&action)) {
        if (transfer->host.empty() || transfer->host.size() > 255 || transfer->port < 1 || transfer->port > 65535)
            return false;
        frame(out, play::TransferPacket::kId, play::TransferPacket{transfer->host, transfer->port});
        return send();
    }
    const auto& store = std::get<HudStoreCookie>(action);
    if (store.key.empty() || store.key.size() > 256 || store.data.size() > wire::protocol::cookie::kMaxPayloadBytes)
        return false;
    if (!cookieCache_.store(store.key, std::span<const std::uint8_t>{store.data}))
        return false;
    frame(out, kPlayCookieStoreId, wire::protocol::cookie::StorePacket{store.key, store.data});
    return send();
}

bool ClientSession::requestPluginCookie(std::string_view key,
                                        std::function<void(std::optional<std::vector<std::uint8_t>>)> reply) {
    if (!inPlay() || closing_ || clientProtocol_ != 0 || legacyRaw_ || key.empty() || key.size() > 256 ||
        pluginCookies_.size() >= kMaxPluginCookieRequests)
        return false;
    wire::io::ByteBuffer out;
    wire::protocol::varInt::write(out, kPlayCookieRequestId);
    wire::protocol::cookie::RequestPacket{std::string{key}}.write(out);
    if (!queuePacket(out.readable()))
        return false;
    pluginCookies_.emplace_back(std::string{key}, std::move(reply));
    return true;
}

} // namespace basalt::proxy::runtime::detail
