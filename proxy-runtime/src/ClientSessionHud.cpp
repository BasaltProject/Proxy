#include "ClientSession.hpp"

#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>
#include <basalt/wire/protocol/play/BorderPackets.hpp>
#include <basalt/wire/protocol/play/HudPackets.hpp>
#include <basalt/wire/protocol/play/UiPackets.hpp>

#include <algorithm>

namespace basalt::proxy::runtime::detail {
namespace {

namespace play = wire::protocol::play;

constexpr std::int32_t kPlayCookieRequestId = 0x15;
constexpr std::int32_t kPlayCookieStoreId = 0x7a;
constexpr std::size_t kMaxPluginCookieRequests = 8;

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

bool ClientSession::applyHud(const PlayerHud& action) {
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
