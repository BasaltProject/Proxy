#include "ClientSession.hpp"

#include "BackendDial.hpp"
#include "BackendLogin.hpp"
#include "BackendRoutes.hpp"
#include "LoginShapes.hpp"
#include "NativeForwarding.hpp"
#include "RelayCounters.hpp"
#include "RouteVersions.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/auth/SessionKeyPair.hpp>
#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/common/placeholder/Placeholders.hpp>
#include <basalt/wire/io/ProxyProtocol.hpp>
#include <basalt/wire/multiversion/ProtocolVersions.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>
#include <basalt/wire/protocol/handshake/HandshakePacket.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/play/ChatPackets.hpp>
#include <basalt/wire/protocol/play/CommandPackets.hpp>
#include <basalt/wire/protocol/play/HudPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>
#include <basalt/wire/text/ComponentJson.hpp>
#include <basalt/wire/text/MiniMessage.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace basalt::proxy::runtime::detail {
namespace proxyProtocol = wire::io::proxyProtocol;

namespace {
constexpr std::int32_t kConfigurationAcknowledgedId = 0x10;
constexpr std::size_t kClientSettingsPacketLimit = 96;
constexpr std::int32_t kLanguageEncodedLimit = 16 * 3;
} // namespace

ClientSession::ClientSession(
    wire::io::TcpConnection connection, wire::io::ResourceBudget::Reservation slot,
    wire::io::ResourceBudget::Reservation buffers, const config::RuntimeConfig& settings, StatusSource status,
    std::span<const std::uint8_t> unavailable, Clock::time_point acceptedAt, bool online, bool checkAddress,
    std::span<const std::uint8_t> publicKey, std::span<const std::uint8_t> authFailure,
    AuthenticationStart authenticate, const BackendRoutes& routes, ResolutionStart resolve,
    const std::vector<config::BackendConfig>& backends, wire::io::ResourceBudget globalBuffers,
    std::int32_t compressionThreshold, bool acceptTransfers, IdentityRegistry& identities,
    const NativeForwarding* nativeForwarding, std::shared_ptr<const void> topology,
    std::function<bool(std::string_view, std::string_view)> playerCommand,
    std::function<std::vector<PlayerCommandSpec>(std::string_view)> playerCommandSpecs,
    config::TranslationMode translation, std::function<void(std::string_view, std::string_view)> playerChat,
    std::function<void(std::string_view, std::string_view)> playerKick,
    std::function<void(std::string_view, std::int32_t)> statusPing,
    std::function<std::optional<std::string>(std::string_view, std::string_view, std::string_view)> playerLogin,
    std::function<std::string(std::string_view)> kickText,
    std::function<std::optional<std::vector<std::string>>(std::string_view, std::string_view)> commandSuggestions,
    std::function<std::optional<std::string>()> loadingMessage, ClientAccess access,
    std::function<bool(std::string_view, std::string_view, std::span<const std::uint8_t>, bool)> pluginMessage,
    std::function<bool(DecisionRequest)> decide, std::function<void(std::string_view, std::int32_t)> latency)
    : translationMode_(translation), topology_(std::move(topology)), playerCommand_(std::move(playerCommand)),
      pluginMessage_(std::move(pluginMessage)), decide_(std::move(decide)), latency_(std::move(latency)),
      playerChat_(std::move(playerChat)), playerKick_(std::move(playerKick)), statusPing_(std::move(statusPing)),
      playerLogin_(std::move(playerLogin)), kickText_(std::move(kickText)), loadingMessage_(std::move(loadingMessage)),
      playerCommandSpecs_(std::move(playerCommandSpecs)), commandSuggestions_(std::move(commandSuggestions)),
      slot_(std::move(slot)), localBuffers_(settings.sessionBufferBytes),
      memory_(std::move(globalBuffers), localBuffers_), pendingCookies_(memory_), pendingKeepAlives_(memory_),
      pendingPings_(memory_, 0x7fffffff), cookieCache_(memory_), resourcePacks_(memory_), identities_(identities),
      nativeForwarding_(nativeForwarding), connection_(std::move(connection)), settings_(settings),
      status_(std::move(status)), tail_(0, memory_), unavailable_(unavailable), online_(online),
      checkAddress_(checkAddress), acceptTransfers_(acceptTransfers), compressionThreshold_(compressionThreshold),
      publicKey_(publicKey), authFailure_(authFailure), authenticate_(std::move(authenticate)), routes_(routes),
      resolve_(std::move(resolve)), backends_(backends), uiFrame_(0, memory_), clientSettings_(0, memory_),
      idIn_(0, memory_), idOut_(0, memory_), input_(0, memory_), frame_(0, memory_), output_(0, memory_),
      readDeadline_(acceptedAt + std::chrono::milliseconds{settings.handshakeTimeoutMs}), access_(std::move(access)) {
    buffers.reset();
    if (access_.policy) {
        limiter_ = wire::protocol::PacketLimiter{access_.settings->packetLimits};
        chatSpam_.configure(access_.settings->limits.chatSpamThresholdSeconds);
        commandSpam_.configure(access_.settings->limits.commandSpamThresholdSeconds);
        if (access_.awaitHeader)
            state_ = State::kProxyHeader;
    }
    allocationReady_ =
        input_.tryReserve(kBufferCapacity) && frame_.tryReserve(kBufferCapacity) && output_.tryReserve(kBufferCapacity);
}

bool ClientSession::inPlay() const {
    return state_ == State::kPlay && activeBackend_ && !recovering_ && !closing_ && profile_;
}

std::string_view ClientSession::activeBackendName() const {
    return activeBackend_ && activeBackend_->attempt().target < backends_.size()
               ? std::string_view{backends_[activeBackend_->attempt().target].name}
               : std::string_view{};
}

std::string_view ClientSession::playerName() const {
    return profile_ ? std::string_view{profile_->name} : std::string_view{};
}

bool ClientSession::accounted() const {
    return allocationReady_ &&
           input_.capacity() + frame_.capacity() + output_.capacity() + tail_.capacity() + uiFrame_.capacity() +
                   pendingCookies_.allocated() + pendingKeepAlives_.allocated() + pendingPings_.allocated() +
                   cookieCache_.allocated() + resourcePacks_.allocated() + clientSettings_.capacity() +
                   idIn_.capacity() + idOut_.capacity() + (activeBackend_ ? activeBackend_->allocated() : 0) +
                   (candidateBackend_ ? candidateBackend_->allocated() : 0) <=
               localBuffers_.used();
}

ClientSession::Clock::time_point ClientSession::deadline() const {
    const auto base = baseDeadline();
    return closing_ ? base : std::min(base, uiDeadline_);
}

ClientSession::Clock::time_point ClientSession::baseDeadline() const {
    auto result = closing_ ? writeDeadline_
                           : std::min(frontendPaused_ ? Clock::time_point::max() : readDeadline_, writeDeadline_);
    if (!closing_ &&
        (connectionStage_ == ConnectionStage::kResolving || connectionStage_ == ConnectionStage::kConnecting))
        result = std::min(result, connectDeadline_);
    if (!closing_ && recovering_)
        result = std::min(result, loginDeadline_);
    if (!closing_ && decisionPending())
        result = std::min(result, decisionDeadline_);
    return closing_ ? result : std::min({result, backendDeadline(), joinRetryAt_});
}

wire::io::IoInterest ClientSession::interest() const {
    return {!closing_ && !frontendPaused_ && !deciding_ && state_ != State::Authenticating &&
                state_ != State::kBackendLogin,
            output_.readableBytes() != 0 || (!frontendPaused_ && bufferedPacket())};
}

bool ClientSession::queue(std::span<const std::uint8_t> bytes) {
    output_.discardReadBytes();
    if (bytes.size() > kBufferCapacity - output_.writerIndex())
        return false;
    if (output_.readableBytes() == 0)
        writeDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs};
    const auto offset = output_.writerIndex();
    output_.writeBytes(bytes);
    relayCounters().towardClients.fetch_add(output_.writerIndex() - offset, std::memory_order_relaxed);
    if (encryptor_)
        encryptor_->encrypt(output_.mutableSpan(offset));
    if (output_.readableBytes() >= settings_.highWaterBytes)
        backendPaused_ = true;
    return true;
}

void ClientSession::refillFromTail() {
    if (tail_.readableBytes() == 0)
        return;
    output_.discardReadBytes();
    const auto count = std::min(kBufferCapacity - output_.writerIndex(), tail_.readableBytes());
    if (count == 0)
        return;
    if (output_.readableBytes() == 0)
        writeDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs};
    output_.writeBytes(tail_.readable().first(count));
    relayCounters().towardClients.fetch_add(count, std::memory_order_relaxed);
    tail_.skip(count);
    if (tail_.readableBytes() == 0)
        tail_.clear();
}

bool ClientSession::queueOrdered(std::span<const std::uint8_t> bytes) {
    constexpr std::size_t kTailLimit = 256 * 1024;
    output_.discardReadBytes();
    if (tail_.readableBytes() == 0 && bytes.size() <= kBufferCapacity - output_.writerIndex())
        return queue(bytes);
    tail_.discardReadBytes();
    if (tail_.readableBytes() + bytes.size() > kTailLimit || !tail_.tryReserve(bytes.size()))
        return false;
    tail_.writeBytes(bytes);
    refillFromTail();
    return accounted();
}

bool ClientSession::rememberSettings(std::span<const std::uint8_t> packet) {
    // parity: ClientInformation bounds the encoded language at 16 * 3 bytes.
    if (packet.size() > kClientSettingsPacketLimit)
        return false;
    const auto payload = frame_.readable();
    const auto languageBytes = wire::protocol::varInt::read(frame_);
    if (frame_.failed() || languageBytes < 0 || languageBytes > kLanguageEncodedLimit)
        return false;
    (void)frame_.readSpan(static_cast<std::size_t>(languageBytes));
    (void)frame_.readByte();
    (void)wire::protocol::varInt::read(frame_);
    (void)wire::protocol::readBoolean(frame_);
    (void)frame_.readByte();
    (void)wire::protocol::varInt::read(frame_);
    (void)wire::protocol::readBoolean(frame_);
    (void)wire::protocol::readBoolean(frame_);
    if (!rawNow() || clientProtocol_ >= wire::multiversion::protocolVersions::kV1_21_2)
        (void)wire::protocol::varInt::read(frame_);
    if (frame_.failed() || frame_.readableBytes() != 0 || !clientSettings_.tryReserve(packet.size()))
        return false;
    clientSettings_.clear();
    wire::protocol::varInt::write(clientSettings_, wire::protocol::configuration::ClientInformationPacket::kId);
    clientSettings_.writeBytes(payload);
    return true;
}

bool ClientSession::packet() {
    namespace protocol = wire::protocol;
    auto raw = frame_.readable();
    const auto id = protocol::varInt::read(frame_);
    if (frame_.failed())
        return false;
    if (limboState())
        return limboPacket(id, raw);
    const bool chatTapped = state_ == State::kPlay && !legacyRaw_ && isChatPacket(id);
    const std::vector<std::uint8_t> chatOriginal =
        chatTapped ? std::vector<std::uint8_t>{frame_.readable().begin(), frame_.readable().end()}
                   : std::vector<std::uint8_t>{};
    switch (tap(frame_, true, id, raw)) {
    case Tapped::kDrop:
        if (chatTapped)
            return applyChatVerdict(denyChat(id, chatOriginal));
        return true;
    case Tapped::kFailed:
        return false;
    case Tapped::kChanged:
        if (chatTapped) {
            const auto verdict = checkReplacedChat(id, chatOriginal, frame_.readable());
            if (verdict.kind != ChatVerdict::Kind::kForward)
                return applyChatVerdict(verdict);
        }
        break;
    default:
        break;
    }
    if (state_ == State::kCookieResponse)
        return id == protocol::cookie::ResponsePacket::kLoginId && cookieResponse();
    if (state_ == State::kLoginAck) {
        if (id != protocol::login::kLoginAcknowledgedId || frame_.readableBytes() != 0 ||
            (output_.readableBytes() != 0 && !synthesizing_) || !backend())
            return false;
        if (!toBackend(*backend(), raw))
            return false;
        backpressure();
        if (!backend()->acknowledgeLogin())
            return false;
        state_ = State::kConfiguration;
        readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
        backend()->expectReadUntil(readDeadline_);
        backend()->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
        if (legacyRaw_) {
            constexpr std::array<std::uint8_t, 1> finish{
                static_cast<std::uint8_t>(wire::protocol::configuration::kFinishConfigurationId)};
            return backendFrame(finish);
        }
        return true;
    }
    if (state_ == State::kConfiguration || state_ == State::kPlay) {
        if (!backend() || id < 0 || (state_ == State::kConfiguration && id == kConfigurationAcknowledgedId))
            return false;
        if (access_.policy && limiter_.active())
            if (const auto handled = limited(id))
                return *handled;
        const bool shaped = !legacyRaw_;
        if (shaped && access_.policy && profile_)
            if (const auto handled = screenExploits(id))
                return *handled;
        if (shaped && state_ == State::kPlay && access_.policy && profile_)
            if (const auto handled = screenChat(id))
                return *handled;
        if (shaped && state_ == State::kPlay && playerCommand_ && profile_ &&
            (id == protocol::play::ChatCommandPacket::kId || id == protocol::play::SignedChatCommandPacket::kId)) {
            wire::io::ByteBuffer view;
            view.writeBytes(frame_.readable());
            const std::string command = protocol::readString(view, 256);
            const auto root = command.substr(0, command.find(' '));
            const bool theBackends =
                root.find(':') == std::string::npos &&
                std::ranges::find(backendCommands_, wire::common::placeholder::lowercaseAscii(
                                                        root.starts_with('/') ? root.substr(1) : root)) !=
                    backendCommands_.end();
            if (!view.failed() && !theBackends && playerCommand_(profile_->name, command))
                return applyChatVerdict(denyChat(id, frame_.readable()));
            if (!view.failed() && !deciding_ &&
                ask(DecisionKind::kCommand, {}, activeBackendName(), command,
                    carriesSignature(id, frame_.readable()))) {
                held_.assign(raw.begin(), raw.end());
                deciding_ = true;
                return true;
            }
        }
        if (shaped && state_ == State::kPlay && commandSuggestions_ && profile_ &&
            id == protocol::play::CommandSuggestionRequestPacket::kId) {
            wire::io::ByteBuffer view;
            view.writeBytes(frame_.readable());
            const auto request = protocol::play::CommandSuggestionRequestPacket::read(view);
            if (request && !view.failed() && answerSuggestions(*request))
                return true;
        }
        if (shaped && state_ == State::kPlay && playerChat_ && profile_ &&
            id == protocol::play::ChatMessagePacket::kId) {
            wire::io::ByteBuffer view;
            view.writeBytes(frame_.readable());
            const std::string message = protocol::readString(view, protocol::play::ChatMessagePacket::kMaxLength);
            if (!view.failed()) {
                playerChat_(profile_->name, message);
                if (ask(DecisionKind::kChat, {}, activeBackendName(), message,
                        carriesSignature(id, frame_.readable()))) {
                    held_.assign(raw.begin(), raw.end());
                    deciding_ = true;
                    return true;
                }
            }
        }
        if (id == (state_ == State::kConfiguration ? protocol::configuration::KeepAlivePacket::kId
                                                   : protocol::play::PlayKeepAlivePacket::kServerboundId))
            return challengeResponse();
        if (shaped && id == (state_ == State::kConfiguration ? 0x05 : 0x2d))
            return challengeResponse(true);
        if (id == (state_ == State::kConfiguration ? 0x02 : 0x16)) {
            const auto result = pluginMessage(false);
            if (result != PluginMessageResult::kRelay)
                return result == PluginMessageResult::kConsumed;
        }
        const auto settingsId = state_ == State::kConfiguration ? 0x00 : 0x0e;
        if (shaped && id == settingsId && !rememberSettings(raw))
            return false;
        const auto cookieResponseId = state_ == State::kConfiguration ? 0x01 : 0x15;
        if (shaped && id == cookieResponseId)
            return cookieResponse();
        if (shaped && id == (state_ == State::kConfiguration ? 0x06 : 0x32))
            return resourcePackResponse();
        bool enterPlay = false;
        bool enterConfiguration = false;
        if (state_ == State::kPlay && id == kConfigurationAcknowledgedId) {
            if (!startingConfiguration_ || frame_.readableBytes() != 0 ||
                (output_.readableBytes() != 0 && !synthesizing_))
                return false;
            if (switchingBackend_)
                return acknowledgePreparedChange();
            pendingKeepAlives_.clear();
            pendingPings_.clear();
            startingConfiguration_ = false;
            finishingConfiguration_ = false;
            state_ = State::kConfiguration;
            enterConfiguration = true;
        } else if (state_ == State::kConfiguration && id == protocol::configuration::kFinishConfigurationId) {
            if (!finishingConfiguration_ || frame_.readableBytes() != 0 ||
                (output_.readableBytes() != 0 && !synthesizing_))
                return false;
            enterPlay = true;
        }
        if (recovering_)
            return true;
        if (!toBackend(*backend(), raw))
            return false;
        if (enterConfiguration && !backend()->beginConfiguration())
            return false;
        if (enterConfiguration && clientSettings_.readableBytes() != 0 &&
            !toBackend(*backend(), clientSettings_.readable()))
            return false;
        if (enterPlay) {
            pendingKeepAlives_.clear();
            pendingPings_.clear();
            if (!backend()->finishConfiguration())
                return false;
            if (!activeBackend_) {
                activeBackend_ = std::move(candidateBackend_);
                relayCounters().logins.fetch_add(1, std::memory_order_relaxed);
            }
            state_ = State::kPlay;
            awaitingJoin_ = legacyRaw_;
            startSecureChat();
            registerBungeeChannels();
        }
        backpressure();
        backend()->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
        readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
        return true;
    }
    return preLoginPacket(id);
}

bool ClientSession::decode() {
    while (input_.readableBytes() != 0 && !closing_ && !frontendPaused_ && !deciding_ &&
           state_ != State::Authenticating && state_ != State::kBackendLogin) {
        if (state_ == State::kProxyHeader) {
            const auto header = proxyProtocol::parse(input_.readable());
            if (header.status == proxyProtocol::Status::kNeedMore)
                break;
            if (header.status == proxyProtocol::Status::kInvalid)
                return false;
            input_.skip(header.consumed);
            if (!acceptOrigin(header.address, header.port))
                return false;
            continue;
        }
        const bool relay = state_ == State::kConfiguration || state_ == State::kPlay;
        const std::size_t limboCap = limboState() && access_.policy
                                         ? static_cast<std::size_t>(access_.policy->antibot.config()->maxFrameBytes)
                                         : kBufferCapacity - 3;
        const auto hint = wire::protocol::packetFraming::inspectFrame(
            input_.readable(), compression_.has_value(),
            relay                              ? wire::protocol::packetFraming::kMaxUncompressedSize
            : state_ == State::kCookieResponse ? wire::protocol::cookie::kMaxPayloadBytes + 512
                                               : limboCap,
            compression_);
        if (!hint)
            return false;
        if (!*hint)
            break;
        const auto sizes = **hint;
        if (sizes.frameBytes > input_.capacity() && !input_.tryReserve(sizes.frameBytes - input_.writerIndex()))
            return false;
        if (!sizes.complete)
            break;
        frame_.clear();
        if (!sizes.packetBytes || (*sizes.packetBytes > frame_.capacity() && !frame_.tryReserve(*sizes.packetBytes)))
            return false;
        auto decoded =
            wire::protocol::packetFraming::tryReadFrame(input_, compression_.has_value(), frame_, compression_);
        if (!decoded)
            return false;
        if (!*decoded)
            break;
        const bool raw = numbered(protocolState());
        if (raw) {
            if (ids_->apply(IdRenumbering::Flow::kClientToSession, protocolState(), frame_.readable(), idIn_) !=
                IdRenumbering::Outcome::kRenumbered)
                return false;
            frame_.clear();
            if (!frame_.tryReserve(idIn_.readableBytes() + 8))
                return false;
            frame_.writeBytes(idIn_.readable());
        }
        if (raw || !translation_ || (rawNow() && !legacyRaw_) ? !packet() : !translateInbound())
            return false;
    }
    input_.discardReadBytes();
    return true;
}

bool ClientSession::flushOutput() {
    if (output_.readableBytes() == 0)
        return true;
    const auto sent = connection_.writeSome(output_.readable());
    if (!sent)
        return sent.error().kind == wire::io::SocketError::Kind::WouldBlock;
    output_.skip(*sent);
    if (output_.readableBytes() == 0) {
        output_.clear();
        writeDeadline_ = Clock::time_point::max();
        refillFromTail();
    }
    return true;
}

bool ClientSession::ready(const wire::io::IoReady& event) {
    if (Clock::now() >= deadline())
        return false;
    const bool waiting = deciding_ || state_ == State::Authenticating || state_ == State::kBackendLogin;
    if (waiting && event.closed)
        return false;
    if (frontendPaused_ && event.closed)
        return false;
    if (!closing_ && !frontendPaused_ && !waiting && bufferedPacket()) {
        if (!decode())
            return false;
    }
    if (!closing_ && !frontendPaused_ && !waiting && (event.readable || event.closed)) {
        thread_local std::array<std::uint8_t, kBufferCapacity> chunk;
        const auto room = input_.capacity() - input_.writerIndex();
        if (room == 0)
            return false;
        const auto read = connection_.read(std::span{chunk}.first(std::min(room, chunk.size())));
        if (!read) {
            if (read.error().kind != wire::io::SocketError::Kind::WouldBlock)
                return false;
        } else if (*read == 0) {
            closing_ = true;
        } else {
            if (decryptor_)
                decryptor_->decrypt(std::span{chunk}.first(*read));
            input_.writeBytes(std::span{chunk}.first(*read));
            if (!decode())
                return false;
        }
    }
    if (!flushOutput())
        return false;
    backpressure();
    return accounted() && (!closing_ || output_.readableBytes() != 0) && advancePreparedChange();
}

bool ClientSession::authenticated(
    std::expected<wire::auth::AuthenticatedLogin, wire::auth::LoginAuthenticationFailure> result,
    wire::io::ResourceBudget::Reservation storage) {
    if (state_ != State::Authenticating || Clock::now() >= loginDeadline_ || input_.readableBytes() != 0 ||
        output_.readableBytes() != 0)
        return false;
    std::optional<wire::auth::LoginSecret> secret = result ? std::optional{result->secret} : result.error().secret;
    if (!secret)
        return false;
    encryptor_ = wire::auth::Cfb8Cipher::create(*secret);
    decryptor_ = wire::auth::Cfb8Cipher::create(*secret);
    if (!encryptor_ || !decryptor_)
        return false;
    authentication_.reset();
    if (result) {
        profileStorage_ = std::move(storage);
        profile_ = std::move(result->profile);
        std::size_t retained =
            profile_->name.capacity() + profile_->properties.capacity() * sizeof(wire::auth::SessionProfileProperty);
        for (const auto& property : profile_->properties) {
            retained += property.name.capacity() + property.value.capacity();
            if (property.signature)
                retained += property.signature->capacity();
        }
        if (!profileStorage_->resize(retained) || !profileLocal_ || !profileLocal_->resize(retained))
            return false;
    }
    if (result) {
        if (playerLogin_)
            if (const auto turnedAway = playerLogin_(profile_->name, profile_->id.toString(), clientAddress()))
                return refuseMiniMessage(*turnedAway);
        return afterLogin();
    }
    if (access_.policy)
        access_.policy->antibot.loginFailed(origin(), Clock::now());
    closing_ = true;
    return refuseWith("login-failed", "Login could not be completed.");
}

bool ClientSession::timedOut(Clock::time_point now) {
    if (now < deadline())
        return true;
    if (limboRun_ && !closing_)
        return now < writeDeadline_ && limboTimer(now);
    if (!closing_ && now >= uiDeadline_ && now < baseDeadline()) {
        uiTick(now);
        return true;
    }
    if (!closing_ && joinRetryAt_ != Clock::time_point::max() && now >= joinRetryAt_)
        return beginJoin();
    if (!closing_ && decisionPending() && now >= decisionDeadline_)
        return decided(decisionToken_, {}, true);
    if (!closing_ &&
        (connectionStage_ == ConnectionStage::kResolving || connectionStage_ == ConnectionStage::kConnecting) &&
        now >= connectDeadline_ && now < loginDeadline_ && now < readDeadline_ && now < writeDeadline_ &&
        now < backendDeadline())
        return route();
    if (!closing_ && activeBackend_ && state_ == State::kPlay && !startingConfiguration_ &&
        now < (frontendPaused_ ? Clock::time_point::max() : readDeadline_) && now < writeDeadline_) {
        if (!recovering_ && now >= activeBackend_->deadline(backendPaused_))
            return recoverBackend();
        if (recovering_ && candidateBackend_ && now < loginDeadline_ && now >= candidateBackend_->deadline()) {
            candidateBackend_.reset();
            return route();
        }
    }
    if (!closing_ && recovering_ && now >= loginDeadline_)
        return !startingConfiguration_ && recoveryFailed();
    if (!closing_ && !recovering_ && activeBackend_ && state_ == State::kPlay && !startingConfiguration_ &&
        (candidateBackend_ || connectionStage_ != ConnectionStage::kIdle) &&
        now < (frontendPaused_ ? Clock::time_point::max() : readDeadline_) && now < writeDeadline_ &&
        now < activeBackend_->deadline(backendPaused_)) {
        discardCandidate();
        return true;
    }
    return false;
}

} // namespace basalt::proxy::runtime::detail

namespace basalt::proxy::runtime::detail {

const std::string& ClientSession::clientAddress() const {
    return originAddress_.empty() ? connection_.remoteAddress() : originAddress_;
}

std::uint16_t ClientSession::clientPort() const {
    return originAddress_.empty() ? connection_.remotePort() : originPort_;
}

bool ClientSession::acceptOrigin(std::string_view address, std::uint16_t port) {
    auto& policy = *access_.policy;
    if (!address.empty()) {
        originAddress_ = address;
        originPort_ = port;
    }
    if (policy.admission.attempt(clientAddress(), Clock::now(), policy.antibot.knownOrigin(origin(), Clock::now())) !=
        AdmissionResult::Accepted)
        return false;
    if (policy.antibot.connection(origin(), Clock::now()).action == antibot::Action::kDeny)
        return false;
    access_.slot = policy.gate.open(clientAddress(), access_.settings->network.maxConnectionsPerAddress);
    if (!access_.slot)
        return false;
    state_ = State::Handshake;
    return true;
}

bool ClientSession::beginJoin() {
    if (!access_.policy)
        return route();
    if (pendingLimbo_ != antibot::Action::kAllow)
        return startLimbo();
    return continueJoin();
}

bool ClientSession::continueJoin() {
    const auto wait = access_.policy->gate.joinWait(access_.settings->network.maxJoinsPerTick, Clock::now());
    joinRetryAt_ = wait.value_or(Clock::time_point::max());
    if (!wait)
        return route();
    state_ = State::kBackendLogin;
    return true;
}

bool ClientSession::kickClient(std::string_view key, std::string_view fallback) {
    if (!inPlay())
        return false;
    const std::string text = kickText_ ? kickText_(key) : std::string{};
    kick(text.empty() ? fallback : std::string_view{text});
    return true;
}

std::optional<bool> ClientSession::limited(std::int32_t id) {
    using Verdict = wire::protocol::PacketLimiter::Verdict;
    const auto phase = state_ == State::kPlay ? wire::protocol::packetNames::Phase::kPlay
                                              : wire::protocol::packetNames::Phase::kConfiguration;
    switch (limiter_.receive(phase, legacyRaw_ ? -1 : id, Clock::now())) {
    case Verdict::kAllow:
        break;
    case Verdict::kDrop:
        return true;
    case Verdict::kKick:
        return kickClient("packet-rate", "Kicked for exceeding packet rate limit");
    }
    return std::nullopt;
}

} // namespace basalt::proxy::runtime::detail
