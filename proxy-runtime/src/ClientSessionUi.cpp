#include "ClientSession.hpp"

#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/play/UiPackets.hpp>

namespace basalt::proxy::runtime::detail {
namespace {

namespace play = wire::protocol::play;
using wire::config::UiElement;

constexpr std::chrono::milliseconds kUiTick{50};
constexpr std::chrono::seconds kUiIdleCheck{1};

struct Owned {
    UiElement element;
    wire::config::UiPriority priority;
    bool enabled;
};

std::optional<UiElement> elementOf(std::int32_t id) {
    switch (id) {
    case play::TabListPacket::kId:
        return UiElement::kTabList;
    case play::BossEventPacket::kId:
        return UiElement::kBossBar;
    case play::SetObjectivePacket::kId:
    case play::SetDisplayObjectivePacket::kId:
    case play::SetScorePacket::kId:
    case play::ResetScorePacket::kId:
        return UiElement::kScoreboard;
    default:
        return std::nullopt;
    }
}

} // namespace

bool ClientSession::pluginOwns(wire::config::UiElement element) const {
    switch (element) {
    case UiElement::kTabList:
        return tabHeader_ || tabFooter_;
    case UiElement::kScoreboard:
        return sidebarShown_;
    case UiElement::kBossBar:
        return false;
    }
    return false;
}

bool ClientSession::uiBackendPacket(std::int32_t id) {
    if (clientProtocol_ != 0 || rawNow() || !uiSource_.configs)
        return false;
    if (id == play::LoginPlayPacket::kId) {
        uiReady_ = true;
        uiDeadline_ = Clock::now() + kUiTick;
        return false;
    }
    const auto element = elementOf(id);
    if (!element)
        return false;
    const auto configs = uiSource_.configs();
    if (!configs)
        return false;
    wire::config::UiPriority priority = wire::config::UiPriority::kBackend;
    bool enabled = false;
    switch (*element) {
    case UiElement::kTabList:
        priority = configs->tabList.priority;
        enabled = configs->tabList.enabled;
        break;
    case UiElement::kBossBar:
        priority = configs->bossBar.priority;
        enabled = configs->bossBar.enabled;
        break;
    case UiElement::kScoreboard:
        priority = configs->scoreboard.priority;
        enabled = configs->scoreboard.enabled;
        break;
    }
    if (pluginOwns(*element) && priority == wire::config::UiPriority::kProxy)
        return true;
    if (!enabled)
        return false;
    if (priority == wire::config::UiPriority::kProxy)
        return true;
    uiYielded_ |= static_cast<std::uint8_t>(1U << static_cast<unsigned>(*element));
    if (ui_)
        ui_->yield(*element);
    return false;
}

void ClientSession::uiLeavePlay() {
    uiReady_ = false;
    uiYielded_ = 0;
    uiDeadline_ = Clock::time_point::max();
    if (ui_)
        ui_->forget();
}

void ClientSession::uiTick(Clock::time_point now) {
    const auto configs = uiSource_.configs ? uiSource_.configs() : nullptr;
    if (!configs || !uiReady_ || state_ != State::kPlay || startingConfiguration_ || clientProtocol_ != 0 ||
        !profile_) {
        uiDeadline_ = Clock::time_point::max();
        return;
    }
    if (!ui_) {
        if (!configs->anyEnabled()) {
            uiDeadline_ = now + kUiIdleCheck;
            return;
        }
        uiRequest_.emplace(buildInfo::kProtocolVersion, virtualHost_, std::optional<std::string>{clientAddress()},
                           std::optional<std::string>{profile_->name});
        uiContext_ = wire::common::placeholder::PlaceholderContext{}.with(wire::config::pingSlot(), &*uiRequest_);
        ui_ = std::make_unique<wire::config::PlayerUi>(
            uiSource_.configs, *uiSource_.placeholders,
            [this](std::int32_t id, const std::function<void(wire::io::ByteBuffer&)>& write) {
                uiFrame_.clear();
                wire::protocol::varInt::write(uiFrame_, id);
                write(uiFrame_);
                if (!queuePacket(uiFrame_.readable()))
                    uiSendFailed_ = true;
            },
            wire::config::PlayerUi::Overrides{}, wire::common::Logger{"Ui"});
        for (unsigned element = 0; element < 3; element++)
            if ((uiYielded_ & (1U << element)) != 0)
                ui_->yield(static_cast<UiElement>(element));
    }
    if (uiSendFailed_) {
        uiSendFailed_ = false;
        ui_->forget();
        for (unsigned element = 0; element < 3; element++)
            if ((uiYielded_ & (1U << element)) != 0)
                ui_->yield(static_cast<UiElement>(element));
    }
    ui_->tick(uiTick_++, uiContext_);
    if (!configs->anyEnabled()) {
        ui_.reset();
        uiDeadline_ = now + kUiIdleCheck;
        return;
    }
    uiDeadline_ = now + kUiTick;
}

} // namespace basalt::proxy::runtime::detail
