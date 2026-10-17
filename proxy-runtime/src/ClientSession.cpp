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
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>
#include <basalt/wire/text/ComponentJson.hpp>
#include <basalt/wire/text/MiniMessage.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::int32_t kConfigurationAcknowledgedId = 0x10;
constexpr std::size_t kClientSettingsPacketLimit = 96;
constexpr std::int32_t kLanguageEncodedLimit = 16 * 3;
} // namespace

ClientSession::ClientSession(wire::io::TcpConnection connection, wire::io::ResourceBudget::Reservation slot,
                             wire::io::ResourceBudget::Reservation buffers, const config::RuntimeConfig& settings,
                             std::span<const std::uint8_t> status, std::span<const std::uint8_t> unavailable,
                             Clock::time_point acceptedAt, bool online, bool checkAddress,
                             std::span<const std::uint8_t> publicKey, std::span<const std::uint8_t> authFailure,
                             AuthenticationStart authenticate, const BackendRoutes& routes, ResolutionStart resolve,
                             const std::vector<config::BackendConfig>& backends, wire::io::ResourceBudget globalBuffers,
                             std::int32_t compressionThreshold, bool acceptTransfers, IdentityRegistry& identities,
                             const NativeForwarding* nativeForwarding, std::shared_ptr<const void> topology,
                             std::function<bool(std::string_view, std::string_view)> playerCommand,
                             std::function<std::vector<PlayerCommandSpec>()> playerCommandSpecs,
                             config::TranslationMode translation,
                             std::function<void(std::string_view, std::string_view)> playerChat,
                             std::function<void(std::string_view, std::string_view)> playerKick,
                             std::function<void(std::string_view, std::int32_t)> statusPing,
                             std::function<void(std::string_view, std::string_view)> playerLogin)
    : translationMode_(translation), topology_(std::move(topology)), playerCommand_(std::move(playerCommand)),
      playerChat_(std::move(playerChat)), playerKick_(std::move(playerKick)), statusPing_(std::move(statusPing)),
      playerLogin_(std::move(playerLogin)), playerCommandSpecs_(std::move(playerCommandSpecs)), slot_(std::move(slot)),
      localBuffers_(settings.sessionBufferBytes), memory_(std::move(globalBuffers), localBuffers_),
      pendingCookies_(memory_), pendingKeepAlives_(memory_), pendingPings_(memory_, 0x7fffffff), cookieCache_(memory_),
      resourcePacks_(memory_), identities_(identities), nativeForwarding_(nativeForwarding),
      connection_(std::move(connection)), settings_(settings), status_(status), unavailable_(unavailable),
      online_(online), checkAddress_(checkAddress), acceptTransfers_(acceptTransfers),
      compressionThreshold_(compressionThreshold), publicKey_(publicKey), authFailure_(authFailure),
      authenticate_(std::move(authenticate)), routes_(routes), resolve_(std::move(resolve)), backends_(backends),
      clientSettings_(0, memory_), idIn_(0, memory_), idOut_(0, memory_), input_(0, memory_), frame_(0, memory_),
      output_(0, memory_), readDeadline_(acceptedAt + std::chrono::milliseconds{settings.handshakeTimeoutMs}) {
    buffers.reset();
    allocationReady_ =
        input_.tryReserve(kBufferCapacity) && frame_.tryReserve(kBufferCapacity) && output_.tryReserve(kBufferCapacity);
}

ClientSession::~ClientSession() {
    closeBackends();
    if (identityClaimed_)
        identities_.release(profile_->id);
    if (authentication_)
        authentication_->request_stop();
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
    return allocationReady_ && input_.capacity() + frame_.capacity() + output_.capacity() +
                                       pendingCookies_.allocated() + pendingKeepAlives_.allocated() +
                                       pendingPings_.allocated() + cookieCache_.allocated() +
                                       resourcePacks_.allocated() + clientSettings_.capacity() + idIn_.capacity() +
                                       idOut_.capacity() + (activeBackend_ ? activeBackend_->allocated() : 0) +
                                       (candidateBackend_ ? candidateBackend_->allocated() : 0) <=
                                   localBuffers_.used();
}

ClientSession::Clock::time_point ClientSession::deadline() const {
    auto result = closing_ ? writeDeadline_
                           : std::min(frontendPaused_ ? Clock::time_point::max() : readDeadline_, writeDeadline_);
    if (!closing_ &&
        (connectionStage_ == ConnectionStage::kResolving || connectionStage_ == ConnectionStage::kConnecting))
        result = std::min(result, connectDeadline_);
    if (!closing_ && recovering_)
        result = std::min(result, loginDeadline_);
    return closing_ ? result : std::min(result, backendDeadline());
}

wire::io::IoInterest ClientSession::interest() const {
    return {!closing_ && !frontendPaused_ && state_ != State::Authenticating && state_ != State::kBackendLogin,
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

wire::protocol::ProtocolState ClientSession::protocolState() const {
    using wire::protocol::ProtocolState;
    switch (state_) {
    case State::Handshake:
        return ProtocolState::Handshake;
    case State::StatusRequest:
    case State::Ping:
        return ProtocolState::Status;
    case State::kConfiguration:
        return ProtocolState::Configuration;
    case State::kPlay:
        return ProtocolState::Play;
    default:
        return ProtocolState::Login;
    }
}

bool ClientSession::rawProtocol(std::int32_t protocol) const {
    return ids_ && clientProtocol_ != 0 && protocol == clientProtocol_;
}

bool ClientSession::rawNow() const { return backend() && rawProtocol(backend()->protocol()); }

bool ClientSession::numbered(wire::protocol::ProtocolState state) const {
    return rawNow() && (legacyRaw_ ? state == wire::protocol::ProtocolState::Play : IdRenumbering::applies(state));
}

bool ClientSession::queueLoginPacket(std::span<const std::uint8_t> packet) {
    return rawNow() ? queueNative(packet) : queuePacket(packet);
}

bool ClientSession::keepAliveVarInt() const { return legacyRaw_ && keepAliveIsVarInt(clientProtocol_); }

bool ClientSession::queuePacket(std::span<const std::uint8_t> packet) {
    if (numbered(protocolState())) {
        switch (ids_->apply(IdRenumbering::Flow::kSessionToClient, protocolState(), packet, idOut_)) {
        case IdRenumbering::Outcome::kRenumbered:
            return queueNative(idOut_.readable());
        case IdRenumbering::Outcome::kDropped:
            return true;
        default:
            return false;
        }
    }
    return translation_ ? queueTranslated(packet) : queueNative(packet);
}

bool ClientSession::toBackend(BackendLogin& target, std::span<const std::uint8_t> frame) {
    if (rawProtocol(target.protocol())) {
        const auto phase = target.phase();
        if (legacyRaw_ && (phase == BackendLogin::Phase::kConfiguration ||
                           (phase == BackendLogin::Phase::kLoginAccepted && !frame.empty() &&
                            frame[0] == wire::protocol::login::kLoginAcknowledgedId)))
            return true;
        const auto state = phase == BackendLogin::Phase::kPlay            ? wire::protocol::ProtocolState::Play
                           : phase == BackendLogin::Phase::kConfiguration ? wire::protocol::ProtocolState::Configuration
                                                                          : wire::protocol::ProtocolState::Login;
        if (numbered(state)) {
            switch (ids_->apply(IdRenumbering::Flow::kSessionToBackend, state, frame, idOut_)) {
            case IdRenumbering::Outcome::kRenumbered:
                return static_cast<bool>(target.queue(idOut_.readable()));
            case IdRenumbering::Outcome::kDropped:
                return true;
            default:
                return false;
            }
        }
    }
    return static_cast<bool>(target.queue(frame));
}

std::int32_t ClientSession::speaksTo(const config::BackendConfig& backend, std::int32_t client) const {
    constexpr std::int32_t kFirstWithLoginQueries = 393;
    const bool needsQueries = backend.forwarding == wire::auth::forwarding::ForwardingMode::Velocity ||
                              backend.forwarding == wire::auth::forwarding::ForwardingMode::Basalt;
    if (backend.protocol == 0 &&
        (translationMode_ == config::TranslationMode::Proxy ||
         (backend.automatic && needsQueries && client != 0 && client < kFirstWithLoginQueries)))
        return buildInfo::kProtocolVersion;
    return effectiveProtocol(backend.protocol, client);
}

bool ClientSession::routeAllowed(std::int32_t client, const config::BackendConfig& backend) const {
    const std::int32_t speaks = speaksTo(backend, client);
    if (translationMode_ == config::TranslationMode::Off && client != buildInfo::kProtocolVersion &&
        speaks == buildInfo::kProtocolVersion)
        return false;
    return resolveRoute(client, speaks).has_value();
}

bool ClientSession::acceptsBackend(const config::BackendConfig& backend) const {
    constexpr std::int32_t kNative = buildInfo::kProtocolVersion;
    constexpr std::int32_t kFirstWithLoginQueries = 393;
    const std::int32_t protocol = speaksTo(backend, clientProtocol_);
    using Mode = wire::auth::forwarding::ForwardingMode;
    // The signed schemes ride on a login query, which a client before 1.13 cannot carry.
    if ((backend.forwarding == Mode::Velocity || backend.forwarding == Mode::Basalt) && protocol != kNative &&
        protocol < kFirstWithLoginQueries)
        return false;
    if (translationMode_ == config::TranslationMode::Off && clientProtocol_ != 0 && protocol == kNative)
        return false;
    if (legacyRaw_ || (ids_ && !translation_))
        return protocol == clientProtocol_;
    if (ids_)
        return protocol == clientProtocol_ || protocol == kNative;
    return protocol == kNative;
}

bool ClientSession::legacyClient() const {
    return translation_ && clientProtocol_ < wire::multiversion::protocolVersions::kV1_20_2;
}

bool ClientSession::synthesizeClientPacket(std::int32_t id) {
    frame_.clear();
    wire::protocol::varInt::write(frame_, id);
    synthesizing_ = true;
    const bool ok = packet();
    synthesizing_ = false;
    return ok;
}

bool ClientSession::queueTranslated(std::span<const std::uint8_t> packet) {
    namespace protocol = wire::protocol;
    wire::io::ByteBuffer view;
    view.writeBytes(packet);
    const auto id = protocol::varInt::read(view);
    if (view.failed())
        return false;
    const auto body = view.readable();
    bool ok = true;
    translation_->chain().clientbound(protocolState(), id, body,
                                      [&](std::int32_t translatedId, std::span<const std::uint8_t> bytes) {
                                          if (!ok)
                                              return;
                                          wire::io::ByteBuffer whole;
                                          protocol::varInt::write(whole, translatedId);
                                          whole.writeBytes(bytes);
                                          ok = queueNative(whole.readable());
                                      });
    return ok;
}

bool ClientSession::translateInbound() {
    namespace protocol = wire::protocol;
    const auto id = protocol::varInt::read(frame_);
    if (frame_.failed())
        return false;
    const auto readable = frame_.readable();
    const std::vector<std::uint8_t> body{readable.begin(), readable.end()};
    bool ok = true;
    translation_->chain().serverbound(protocolState(), id, body,
                                      [&](std::int32_t translatedId, std::span<const std::uint8_t> bytes) {
                                          if (!ok)
                                              return;
                                          frame_.clear();
                                          if (!frame_.tryReserve(bytes.size() + 8)) {
                                              ok = false;
                                              return;
                                          }
                                          protocol::varInt::write(frame_, translatedId);
                                          frame_.writeBytes(bytes);
                                          ok = packet();
                                      });
    return ok;
}

bool ClientSession::queueNative(std::span<const std::uint8_t> packet) {
    output_.discardReadBytes();
    const auto maximum = compression_ ? wire::protocol::packetFraming::kMaxUncompressedSize
                                      : wire::protocol::packetFraming::kMaxFrameSize;
    if (packet.empty() || packet.size() > static_cast<std::size_t>(maximum) ||
        !output_.tryReserve(packet.size() + packet.size() / 1000 + 64))
        return false;
    if (output_.readableBytes() == 0)
        writeDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs};
    const auto offset = output_.writerIndex();
    auto encoded = compression_ ? wire::protocol::packetFraming::writeCompressed(output_, packet, *compression_)
                                : wire::protocol::packetFraming::writeUncompressed(output_, packet);
    if (!encoded)
        return false;
    relayCounters().towardClients.fetch_add(output_.writerIndex() - offset, std::memory_order_relaxed);
    if (encryptor_)
        encryptor_->encrypt(output_.mutableSpan(offset));
    if (output_.readableBytes() >= settings_.highWaterBytes)
        backendPaused_ = true;
    return true;
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
    const auto raw = frame_.readable();
    const auto id = protocol::varInt::read(frame_);
    if (frame_.failed())
        return false;
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
        const bool shaped = !legacyRaw_;
        if (shaped && state_ == State::kPlay && playerCommand_ && profile_ &&
            (id == protocol::play::ChatCommandPacket::kId || id == protocol::play::SignedChatCommandPacket::kId)) {
            wire::io::ByteBuffer view;
            view.writeBytes(frame_.readable());
            const std::string command = protocol::readString(view, 256);
            if (!view.failed() && playerCommand_(profile_->name, command))
                return true;
        }
        if (shaped && state_ == State::kPlay && playerChat_ && profile_ &&
            id == protocol::play::ChatMessagePacket::kId) {
            wire::io::ByteBuffer view;
            view.writeBytes(frame_.readable());
            const std::string message = protocol::readString(view, protocol::play::ChatMessagePacket::kMaxLength);
            if (!view.failed())
                playerChat_(profile_->name, message);
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
            if (!activeBackend_)
                activeBackend_ = std::move(candidateBackend_);
            state_ = State::kPlay;
            awaitingJoin_ = legacyRaw_;
        }
        backpressure();
        backend()->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
        readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
        return true;
    }
    return preLoginPacket(id);
}

bool ClientSession::decode() {
    while (input_.readableBytes() != 0 && !closing_ && !frontendPaused_ && state_ != State::Authenticating &&
           state_ != State::kBackendLogin) {
        const bool relay = state_ == State::kConfiguration || state_ == State::kPlay;
        const auto hint = wire::protocol::packetFraming::inspectFrame(
            input_.readable(), compression_.has_value(),
            relay                              ? wire::protocol::packetFraming::kMaxUncompressedSize
            : state_ == State::kCookieResponse ? wire::protocol::cookie::kMaxPayloadBytes + 512
                                               : kBufferCapacity - 3,
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

bool ClientSession::ready(const wire::io::IoReady& event) {
    if (Clock::now() >= deadline())
        return false;
    const bool waiting = state_ == State::Authenticating || state_ == State::kBackendLogin;
    if (waiting && event.closed)
        return false;
    if (frontendPaused_ && event.closed)
        return false;
    if (!closing_ && !frontendPaused_ && !waiting && bufferedPacket()) {
        if (!decode())
            return false;
    }
    if (!closing_ && !frontendPaused_ && !waiting && (event.readable || event.closed)) {
        std::array<std::uint8_t, kBufferCapacity> chunk{};
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
    if (output_.readableBytes() != 0) {
        const auto sent = connection_.writeSome(output_.readable());
        if (!sent) {
            if (sent.error().kind != wire::io::SocketError::Kind::WouldBlock)
                return false;
        } else {
            output_.skip(*sent);
            if (output_.readableBytes() == 0) {
                output_.clear();
                writeDeadline_ = Clock::time_point::max();
            }
        }
    }
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
            playerLogin_(profile_->name, profile_->id.toString());
        return route();
    }
    closing_ = true;
    return queue(authFailure_);
}

bool ClientSession::timedOut(Clock::time_point now) {
    if (now < deadline())
        return true;
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

void ClientSession::cancelConnectionAttempt() {
    connectionStage_ = ConnectionStage::kIdle;
    attempts_.clear();
    dial_.reset();
    dialAttempt_.reset();
    resolutionStorage_.reset();
    resolutionLocal_.reset();
    if (resolution_)
        resolution_->request_stop();
    resolution_.reset();
}

bool ClientSession::route() {
    if (routeNext())
        return true;
    if (recovering_)
        return recoveryFailed();
    if (activeBackend_ && state_ == State::kPlay && !closing_) {
        discardCandidate();
        return true;
    }
    return false;
}

bool ClientSession::routeNext() {
    cancelConnectionAttempt();
    if (Clock::now() >= loginDeadline_)
        return false;
    if (!profile_)
        return false;
    if (!identityClaimed_) {
        if (!identities_.claim(profile_->id)) {
            closing_ = true;
            frame_.clear();
            wire::protocol::varInt::write(frame_, wire::protocol::login::LoginDisconnectPacket::kId);
            wire::protocol::login::LoginDisconnectPacket{
                wire::text::Component::text("This player is already connected.")}
                .write(frame_);
            return queuePacket(frame_.readable());
        }
        identityClaimed_ = true;
    }
    while (nextTarget_ < targets_.size() && ((recovering_ && recoveryVisited_.test(targets_[nextTarget_])) ||
                                             !acceptsBackend(backends_[targets_[nextTarget_]])))
        ++nextTarget_;
    if (nextTarget_ == targets_.size()) {
        if (recovering_)
            return recoveryFailed();
        if (activeBackend_ && state_ == State::kPlay) {
            discardCandidate();
            return true;
        }
        closing_ = true;
        return queue(unavailable_);
    }
    if (settings_.sessionBufferBytes <
        kBufferBytes + kResolutionBytes + (profileStorage_ ? profileStorage_->size() : 0))
        return false;
    connectDeadline_ = std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.connectTimeoutMs});
    resolutionLocal_ = localBuffers_.acquire(kResolutionBytes);
    if (!resolutionLocal_)
        return false;
    const auto target = targets_[nextTarget_++];
    if (recovering_)
        recoveryVisited_.set(target);
    auto attempt = attempts_.begin(target);
    if (!attempt)
        return false;
    resolution_ = resolve_(*attempt, connectDeadline_);
    if (!resolution_)
        return false;
    connectionStage_ = ConnectionStage::kResolving;
    if (!activeBackend_)
        state_ = State::kBackendLogin;
    return true;
}

bool ClientSession::resolved(BackendAttempt attempt, std::vector<wire::io::TcpAddress> addresses,
                             wire::io::ResourceBudget::Reservation storage) {
    // A canceled lookup can finish after the same backend has been selected again.
    if (closing_ || connectionStage_ != ConnectionStage::kResolving || !attempts_.matches(attempt))
        return true;
    if (Clock::now() >= deadline())
        return timedOut(Clock::now());
    attempts_.clear();
    resolution_.reset();
    if (addresses.empty()) {
        storage.reset();
        return route();
    }
    if (addresses.size() > 32 || addresses.capacity() > storage.size() / sizeof(wire::io::TcpAddress))
        return false;
    if (!storage.resize(addresses.capacity() * sizeof(wire::io::TcpAddress)) || !resolutionLocal_ ||
        !resolutionLocal_->resize(addresses.capacity() * sizeof(wire::io::TcpAddress)))
        return false;
    resolutionStorage_ = std::move(storage);
    dialAttempt_ = attempt;
    dial_ = std::make_unique<BackendDial>(std::move(addresses), connectDeadline_);
    connectionStage_ = ConnectionStage::kConnecting;
    return dial();
}

bool ClientSession::dial() {
    auto state = dial_->advance();
    if (!state)
        return route();
    if (*state == DialState::kConnecting)
        return true;
    auto connected = dial_->take();
    if (!connected)
        return route();
    dial_.reset();
    connectionStage_ = ConnectionStage::kIdle;
    resolutionStorage_.reset();
    resolutionLocal_.reset();
    if (!dialAttempt_ || dialAttempt_->target >= backends_.size())
        return false;
    candidateBackend_ = std::make_unique<BackendLogin>(std::move(*connected), memory_, *dialAttempt_);
    const std::int32_t speaks = speaksTo(backends_[candidateBackend_->attempt().target], clientProtocol_);
    candidateBackend_->useIds(rawProtocol(speaks) ? ids_.get() : nullptr);
    dialAttempt_.reset();
    if (!profile_ ||
        !candidateBackend_->start(backends_[candidateBackend_->attempt().target], *profile_, virtualHost_, virtualPort_,
                                  connection_.remoteAddress(), connection_.remotePort(), nativeForwarding_, speaks)) {
        if (recovering_)
            return recoveryFailed();
        if (activeBackend_ && state_ == State::kPlay) {
            discardCandidate();
            return true;
        }
        closeBackends();
        closing_ = true;
        return queue(unavailable_);
    }
    if (!activeBackend_)
        state_ = State::kBackendLogin;
    candidateBackend_->boundPreparation(loginDeadline_);
    candidateBackend_->expectWriteUntil(
        std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs}));
    return true;
}

BackendLogin* ClientSession::backend() const {
    // Frontend traffic remains attached to the active backend during candidate preparation.
    return activeBackend_ ? activeBackend_.get() : candidateBackend_.get();
}

BackendLogin* ClientSession::backendFor(BackendAttempt attempt) const {
    for (auto* endpoint : {activeBackend_.get(), candidateBackend_.get()})
        if (endpoint && !(recovering_ && endpoint == activeBackend_.get()) && endpoint->attempt() == attempt)
            return endpoint;
    return nullptr;
}

ClientSession::Clock::time_point ClientSession::backendDeadline() const {
    auto result = Clock::time_point::max();
    const std::array endpoints{activeBackend_.get(), candidateBackend_.get()};
    for (const auto* endpoint : endpoints)
        if (endpoint && !(recovering_ && endpoint == activeBackend_.get()))
            result = std::min(result, endpoint->deadline(endpoint == backend() && backendPaused_));
    return result;
}

void ClientSession::closeBackends() {
    cancelConnectionAttempt();
    candidateBackend_.reset();
    activeBackend_.reset();
}

std::array<ClientSession::BackendWatch, 3> ClientSession::backendWatches() const {
    std::array<BackendWatch, 3> result{};
    if (dial_ && dial_->handle() >= 0)
        result[0] = {dial_->handle(), {false, true}};
    const std::array endpoints{activeBackend_.get(), candidateBackend_.get()};
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        auto* endpoint = endpoints[i];
        if (!endpoint)
            continue;
        const bool selected = endpoint == backend();
        const bool preparing = activeBackend_ && endpoint == candidateBackend_.get() && state_ == State::kPlay;
        const bool read = (selected || (preparing && endpoint->phase() == BackendLogin::Phase::kLogin)) &&
                          !backendPaused_ && !endpoint->hasCookieRequest() && state_ != State::kLoginAck &&
                          state_ != State::kCookieResponse && !startingConfiguration_;
        const auto interest = endpoint->interest(read);
        if ((selected && !backendPaused_) || preparing || interest.readable || interest.writable)
            result[i + 1] = {endpoint->handle(), interest};
    }
    return result;
}

void ClientSession::backpressure() {
    if (awaitingJoin_ || (backend() && backend()->pendingBytes() >= settings_.highWaterBytes))
        frontendPaused_ = true;
    else if (frontendPaused_ && (!backend() || backend()->pendingBytes() <= settings_.lowWaterBytes)) {
        frontendPaused_ = false;
        if (state_ == State::kConfiguration || state_ == State::kPlay)
            readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
    }
    if (output_.readableBytes() >= settings_.highWaterBytes)
        backendPaused_ = true;
    else if (backendPaused_ && output_.readableBytes() <= settings_.lowWaterBytes) {
        backendPaused_ = false;
        if (state_ == State::kConfiguration || state_ == State::kPlay)
            if (backend())
                backend()->expectReadUntil(Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs});
    }
}

bool ClientSession::bufferedPacket() const {
    if (state_ != State::kConfiguration && state_ != State::kPlay)
        return false;
    auto hint = wire::protocol::packetFraming::inspectFrame(
        input_.readable(), compression_.has_value(), wire::protocol::packetFraming::kMaxUncompressedSize, compression_);
    return !hint || (*hint && (**hint).complete);
}

} // namespace basalt::proxy::runtime::detail
