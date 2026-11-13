#include "BackendLogin.hpp"
#include "ClientSession.hpp"
#include "RelayCounters.hpp"
#include "antibot/Antibot.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/multiversion/java/JavaTranslators.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/play/BorderPackets.hpp>
#include <basalt/wire/protocol/play/InventoryPackets.hpp>
#include <basalt/wire/protocol/play/ItemIds.hpp>
#include <basalt/wire/protocol/play/MapPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/play/WorldPackets.hpp>
#include <basalt/wire/text/Component.hpp>
#include <basalt/wire/text/MiniMessage.hpp>

#include <algorithm>
#include <map>
#include <tuple>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::int32_t kCaptchaMapId = 0x5eed;
constexpr std::int16_t kFirstHotbarSlot = 36;
constexpr std::size_t kBrandFrameLimit = 256;
constexpr std::int32_t kConfigurationTransferId = 0x0c;
} // namespace

antibot::OriginKey ClientSession::origin() const { return antibot::originOf(clientAddress()); }

bool ClientSession::queueFramed(std::span<const std::uint8_t> framed) {
    output_.discardReadBytes();
    if (framed.empty() || !output_.tryReserve(framed.size()))
        return false;
    if (output_.readableBytes() == 0)
        writeDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs};
    const auto offset = output_.writerIndex();
    output_.writeBytes(framed);
    relayCounters().towardClients.fetch_add(framed.size(), std::memory_order_relaxed);
    if (encryptor_)
        encryptor_->encrypt(output_.mutableSpan(offset));
    return true;
}

constexpr std::int32_t kFirstPingProtocol = 755;

bool ClientSession::startLimbo() {
    auto& policy = *access_.policy;
    const auto now = Clock::now();
    const auto antibotConfig = policy.antibot.config();
    const auto& config = *antibotConfig;
    const std::int32_t protocol = clientProtocol_ != 0 ? clientProtocol_ : buildInfo::kProtocolVersion;
    limboPackets_ = policy.limbo.forProtocol(protocol, config.dropHeight);
    if (!limboPackets_)
        return refuseAntibot("antibot-legacy");
    limboSlot_ = policy.antibot.enterLimbo(origin(), name_, now);
    if (!limboSlot_)
        return refuseAntibot("antibot-busy");
    std::string answer;
    const auto action = std::exchange(pendingLimbo_, antibot::Action::kAllow);
    if (action == antibot::Action::kCaptcha) {
        if (config.captchaType == config::CaptchaType::kBlocks) {
            blockCaptcha_ = antibot::CaptchaFactory{config.captchaLength}.makeBlocks();
            answer = blockCaptcha_->answer;
        } else {
            captcha_ = antibot::CaptchaFactory{config.captchaLength}.make();
            answer = captcha_->answer;
        }
    }
    limboRun_ = std::make_unique<antibot::LimboRun>(config, now, std::move(answer));
    if (action == antibot::Action::kChatCheck)
        limboRun_->armChatCheck();
    limboRun_->expectEcho(config.checkEcho && protocol >= kFirstPingProtocol);
    if (!translation_ && clientProtocol_ != 0) {
        translation_ = wire::multiversion::java::javaTranslators::chainFor(clientProtocol_);
        limboOwnsChain_ = true;
        if (!translation_)
            return limboFailed(antibot::Reason::kLimboFailed);
    }
    if (compressionThreshold_ >= 0) {
        frame_.clear();
        wire::protocol::varInt::write(frame_, wire::protocol::login::LoginCompressionPacket::kId);
        wire::protocol::login::LoginCompressionPacket{compressionThreshold_}.write(frame_);
        if (!queueNative(frame_.readable()))
            return false;
        compression_ = compressionThreshold_;
    }
    state_ = State::kLimboLogin;
    wire::protocol::login::LoginFinishedPacket finished;
    finished.gameProfile.id = profile_->id;
    finished.gameProfile.name = profile_->name;
    for (const auto& property : profile_->properties)
        finished.gameProfile.properties.push_back({property.name, property.value, property.signature});
    finished.sessionId = wire::common::Uuid::random();
    frame_.clear();
    wire::protocol::varInt::write(frame_, wire::protocol::login::LoginFinishedPacket::kId);
    finished.write(frame_);
    if (!queuePacket(frame_.readable()))
        return false;
    readDeadline_ = limboRun_->nextWake();
    return true;
}

bool ClientSession::limboPacket(std::int32_t id, std::span<const std::uint8_t> raw) {
    const auto now = Clock::now();
    antibot::LimboRun::Step step;
    switch (state_) {
    case State::kLimboLogin:
        if (id != wire::protocol::login::kLoginAcknowledgedId)
            return false;
        step = limboRun_->loginAcknowledged();
        state_ = State::kLimboConfiguration;
        if (!queueFramed(limboPackets_->configIntro))
            return false;
        break;
    case State::kLimboConfiguration:
        if (id == 0 && !rememberSettings(raw))
            return false;
        if (id == 2 && raw.size() <= kBrandFrameLimit && limboBrand_.empty())
            limboBrand_.assign(raw.begin(), raw.end());
        step = limboRun_->configuration(id, frame_);
        if (step.signal == antibot::LimboRun::Signal::kSendConfigRest && !queueFramed(limboPackets_->configRest))
            return false;
        if (step.signal == antibot::LimboRun::Signal::kSendPlayIntro) {
            state_ = State::kLimboPlay;
            if (!queueFramed(limboPackets_->playIntro))
                return false;
            if ((captcha_ || blockCaptcha_ || limboRun_->chatCheck()) && !limboSendCaptcha())
                return false;
            if (!limboKeepAlive(now))
                return false;
        }
        break;
    default:
        step = limboRun_->play(id, frame_, now);
        if (step.outcome == antibot::LimboRun::Outcome::kContinue && !limboKeepAlive(now))
            return false;
        break;
    }
    return limboStep(step);
}

bool ClientSession::limboKeepAlive(Clock::time_point now) {
    if (state_ != State::kLimboPlay)
        return true;
    if (const auto ping = limboRun_->pingDue(now)) {
        wire::io::ByteBuffer out;
        wire::protocol::varInt::write(out, 0x3e);
        out.writeInt(*ping);
        if (!queuePacket(out.readable()))
            return false;
    }
    const auto due = limboRun_->keepAliveDue(now);
    if (!due)
        return true;
    wire::io::ByteBuffer out;
    wire::protocol::varInt::write(out, wire::protocol::play::PlayKeepAlivePacket::kClientboundId);
    wire::protocol::play::PlayKeepAlivePacket{*due}.write(out);
    return queuePacket(out.readable());
}

bool ClientSession::limboStep(const antibot::LimboRun::Step& step) {
    using Outcome = antibot::LimboRun::Outcome;
    using Signal = antibot::LimboRun::Signal;
    if (step.outcome == Outcome::kFailed)
        return limboFailed(step.reason);
    if (step.outcome == Outcome::kPassed)
        return limboPassed();
    if (step.signal == Signal::kSendReconfigure && !queueFramed(limboPackets_->startConfiguration))
        return false;
    if (step.signal == Signal::kNeedCaptcha) {
        const auto antibotConfig = access_.policy->antibot.config();
        blockCaptcha_ = antibot::CaptchaFactory{antibotConfig->captchaLength}.makeBlocks();
        limboRun_->armCaptcha(blockCaptcha_->answer);
        if (!limboSendCaptcha())
            return false;
    }
    if (step.signal == Signal::kWrongCode &&
        !limboSystemChat(limboRun_->chatCheck() ? "antibot-chat-wrong" : "antibot-wrong-code"))
        return false;
    readDeadline_ = limboRun_->nextWake();
    return true;
}

bool ClientSession::limboTimer(Clock::time_point now) {
    const auto step = limboRun_->tick(now);
    if (step.outcome != antibot::LimboRun::Outcome::kContinue)
        return limboStep(step);
    if (!limboKeepAlive(now))
        return false;
    readDeadline_ = limboRun_->nextWake();
    return true;
}

wire::text::Component ClientSession::antibotText(std::string_view key) const {
    std::string text = kickText_ ? kickText_(key, {}) : std::string{};
    if (limboRun_ && limboRun_->chatCheck()) {
        const auto config = access_.policy->antibot.config();
        for (const auto& [token, value] :
             {std::pair<std::string_view, const std::string&>{"{register}", config->chatCheckRegisterCommand},
              {"{login}", config->chatCheckLoginCommand}})
            for (auto at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size()))
                text.replace(at, token.size(), value);
    }
    return wire::text::miniMessage::parse(text.empty() ? std::string_view{key} : std::string_view{text});
}

bool ClientSession::refuseAntibot(std::string_view key) {
    closing_ = true;
    frame_.clear();
    wire::protocol::varInt::write(frame_, wire::protocol::login::LoginDisconnectPacket::kId);
    wire::protocol::login::LoginDisconnectPacket{antibotText(key)}.write(frame_);
    return queuePacket(frame_.readable());
}

bool ClientSession::limboSystemChat(std::string_view key) {
    wire::io::ByteBuffer out;
    wire::protocol::varInt::write(out, wire::protocol::play::SystemChatPacket::kId);
    wire::protocol::play::SystemChatPacket{antibotText(key), false}.write(out);
    return queuePacket(out.readable());
}

bool ClientSession::limboSendCaptcha() {
    namespace play = wire::protocol::play;
    if (limboRun_->chatCheck()) {
        if (!limboSystemChat("antibot-chat-prompt"))
            return false;
        limboRun_->captchaShown(Clock::now());
        return true;
    }
    if (blockCaptcha_) {
        std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, play::SectionBlocksUpdatePacket> sections;
        for (const auto& block : blockCaptcha_->blocks) {
            auto& packet = sections[{block.x >> 4, block.y >> 4, block.z >> 4}];
            packet.sectionX = block.x >> 4;
            packet.sectionY = block.y >> 4;
            packet.sectionZ = block.z >> 4;
            packet.changes.push_back(
                {play::SectionBlocksUpdatePacket::packPosition(block.x, block.y, block.z), block.state});
        }
        for (const auto& [key, packet] : sections) {
            wire::io::ByteBuffer out;
            wire::protocol::varInt::write(out, play::SectionBlocksUpdatePacket::kId);
            packet.write(out);
            if (!queuePacket(out.readable()))
                return false;
        }
        if (!limboSystemChat("antibot-captcha-blocks"))
            return false;
        limboRun_->captchaShown(Clock::now());
        return true;
    }
    static const std::int32_t kMapItem = [] {
        for (std::int32_t id = 0; id < static_cast<std::int32_t>(play::javaItemCount()); ++id)
            if (play::javaItemName(id) == "filled_map")
                return id;
        return 0;
    }();
    play::WireItemStack map;
    map.itemId = kMapItem;
    map.count = 1;
    map.components.emplace_back(play::dataComponent::kMapId, play::ItemComponent::Number{kCaptchaMapId});
    wire::io::ByteBuffer out;
    wire::protocol::varInt::write(out, play::ContainerSetSlotPacket::kId);
    play::ContainerSetSlotPacket{0, 0, kFirstHotbarSlot, map}.write(out);
    if (!queuePacket(out.readable()))
        return false;
    out.clear();
    wire::protocol::varInt::write(out, play::SetHeldSlotPacket::kId);
    play::SetHeldSlotPacket{0}.write(out);
    if (!queuePacket(out.readable()))
        return false;
    out.clear();
    wire::protocol::varInt::write(out, play::MapItemDataPacket::kId);
    play::MapItemDataPacket data;
    data.mapId = kCaptchaMapId;
    data.locked = true;
    data.patch = play::MapColorPatch{
        0, 0, antibot::kMapSide, antibot::kMapSide, {captcha_->colors.begin(), captcha_->colors.end()}};
    data.write(out);
    if (!queuePacket(out.readable()) || !limboSystemChat("antibot-captcha"))
        return false;
    limboRun_->captchaShown(Clock::now());
    return true;
}

bool ClientSession::limboLeave(std::string_view key) {
    namespace protocol = wire::protocol;
    limboRun_.reset();
    limboSlot_.reset();
    closing_ = true;
    frame_.clear();
    const auto reasonText = antibotText(key);
    if (state_ == State::kLimboLogin) {
        protocol::varInt::write(frame_, protocol::login::LoginDisconnectPacket::kId);
        protocol::login::LoginDisconnectPacket{reasonText}.write(frame_);
    } else if (state_ == State::kLimboConfiguration) {
        protocol::varInt::write(frame_, protocol::configuration::ConfigurationDisconnectPacket::kId);
        protocol::configuration::ConfigurationDisconnectPacket{reasonText}.write(frame_);
    } else {
        protocol::varInt::write(frame_, protocol::play::PlayDisconnectPacket::kId);
        protocol::play::PlayDisconnectPacket{reasonText}.write(frame_);
    }
    return queuePacket(frame_.readable());
}

bool ClientSession::limboTransfer() {
    namespace protocol = wire::protocol;
    const auto config = access_.policy->antibot.config();
    const std::string host = config->passthroughTransferHost.empty() ? virtualHost_ : config->passthroughTransferHost;
    const std::int32_t port = config->passthroughTransferPort != 0 ? config->passthroughTransferPort : virtualPort_;
    if (!config->passthroughTransfer || translation_ || host.empty() || host.size() > 255 || port < 1 || port > 65535)
        return false;
    access_.policy->antibot.transferIssued(origin(), Clock::now());
    limboRun_.reset();
    limboSlot_.reset();
    closing_ = true;
    frame_.clear();
    protocol::varInt::write(frame_, kConfigurationTransferId);
    protocol::play::TransferPacket{host, port}.write(frame_);
    return queuePacket(frame_.readable());
}

bool ClientSession::limboFailed(antibot::Reason reason) {
    const auto key = reason == antibot::Reason::kTimeout ? "antibot-timeout" : "antibot-failed";
    access_.policy->antibot.limboFailed(origin(), name_, reason, Clock::now());
    return limboLeave(key);
}

bool ClientSession::limboPassed() {
    const auto exit = access_.policy->antibot.limboPassed(origin(), name_, Clock::now());
    if (exit != antibot::Antibot::LimboExit::kEnter) {
        state_ = State::kLimboConfiguration;
        if (exit == antibot::Antibot::LimboExit::kBusy)
            return limboLeave("antibot-retry");
        return limboTransfer() || limboLeave("antibot-rejoin");
    }
    limboRun_.reset();
    limboSlot_.reset();
    limboPackets_.reset();
    captcha_.reset();
    blockCaptcha_.reset();
    if (limboOwnsChain_)
        translation_.reset();
    else if (translation_)
        translation_ = wire::multiversion::java::javaTranslators::chainFor(clientProtocol_);
    limboOwnsChain_ = false;
    limboHandover_ = true;
    loginDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.loginTimeoutMs};
    readDeadline_ = loginDeadline_;
    return continueJoin();
}

bool ClientSession::completeLimboHandover() {
    limboHandover_ = false;
    state_ = State::kLoginAck;
    if (!synthesizeClientPacket(wire::protocol::login::kLoginAcknowledgedId))
        return false;
    if (clientSettings_.readableBytes() != 0 && !toBackend(*backend(), clientSettings_.readable()))
        return false;
    if (!limboBrand_.empty() && !toBackend(*backend(), limboBrand_))
        return false;
    limboBrand_.clear();
    return true;
}

} // namespace basalt::proxy::runtime::detail
