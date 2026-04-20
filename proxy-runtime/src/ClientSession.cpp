#include "ClientSession.hpp"

#include "BackendDial.hpp"
#include "BackendLogin.hpp"
#include "BackendRoutes.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/auth/SessionKeyPair.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>
#include <basalt/wire/protocol/handshake/HandshakePacket.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>

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
                             std::int32_t compressionThreshold, bool acceptTransfers, IdentityRegistry& identities)
    : slot_(std::move(slot)), localBuffers_(settings.sessionBufferBytes),
      memory_(std::move(globalBuffers), localBuffers_), pendingCookies_(memory_), cookieCache_(memory_),
      identities_(identities), connection_(std::move(connection)), settings_(settings), status_(status),
      unavailable_(unavailable), online_(online), checkAddress_(checkAddress), acceptTransfers_(acceptTransfers),
      compressionThreshold_(compressionThreshold), publicKey_(publicKey), authFailure_(authFailure),
      authenticate_(std::move(authenticate)), routes_(routes), resolve_(std::move(resolve)), backends_(backends),
      clientSettings_(0, memory_), input_(0, memory_), frame_(0, memory_), output_(0, memory_),
      readDeadline_(acceptedAt + std::chrono::milliseconds{settings.handshakeTimeoutMs}) {
    buffers.reset();
    allocationReady_ =
        input_.tryReserve(kBufferCapacity) && frame_.tryReserve(kBufferCapacity) && output_.tryReserve(kBufferCapacity);
}

ClientSession::~ClientSession() {
    if (identityClaimed_)
        identities_.release(profile_->id);
    if (authentication_)
        authentication_->request_stop();
    if (resolution_)
        resolution_->request_stop();
}

bool ClientSession::accounted() const {
    return allocationReady_ && input_.capacity() + frame_.capacity() + output_.capacity() +
                                       pendingCookies_.allocated() + cookieCache_.allocated() +
                                       clientSettings_.capacity() + (activeBackend_ ? activeBackend_->allocated() : 0) +
                                       (candidateBackend_ ? candidateBackend_->allocated() : 0) <=
                                   localBuffers_.used();
}

ClientSession::Clock::time_point ClientSession::deadline() const {
    auto result = closing_ ? writeDeadline_
                           : std::min(frontendPaused_ ? Clock::time_point::max() : readDeadline_, writeDeadline_);
    if (!closing_ && (state_ == State::kResolving || state_ == State::kConnecting))
        result = std::min(result, connectDeadline_);
    return closing_ ? result
                    : std::min({result, backendWriteDeadline_,
                                backendPaused_ ? Clock::time_point::max() : backendReadDeadline_});
}

wire::io::IoInterest ClientSession::interest() const {
    return {!closing_ && !frontendPaused_ && state_ != State::Authenticating && state_ != State::kResolving &&
                state_ != State::kConnecting && state_ != State::kBackendLogin,
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
    if (encryptor_)
        encryptor_->encrypt(output_.mutableSpan(offset));
    if (output_.readableBytes() >= settings_.highWaterBytes)
        backendPaused_ = true;
    return true;
}

bool ClientSession::queuePacket(std::span<const std::uint8_t> packet) {
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
    if (state_ == State::kCookieResponse) {
        if (id != protocol::cookie::ResponsePacket::kLoginId || !backend() || !cookie_)
            return false;
        auto response = protocol::cookie::ResponsePacket::read(frame_);
        if (!response || frame_.readableBytes() != 0 || response->key != *cookie_)
            return false;
        if (!cookieCache_.remember(response->key, response->payload
                                                      ? std::optional{std::span<const std::uint8_t>{*response->payload}}
                                                      : std::nullopt))
            return false;
        if (!backend()->queue(raw, protocol::cookie::kMaxPayloadBytes + 512))
            return false;
        cookie_.reset();
        state_ = State::kBackendLogin;
        backendWriteDeadline_ =
            std::min(backendWriteDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
        backpressure();
        return true;
    }
    if (state_ == State::kLoginAck) {
        if (id != protocol::login::kLoginAcknowledgedId || frame_.readableBytes() != 0 ||
            output_.readableBytes() != 0 || !backend())
            return false;
        if (!backend()->queue(raw))
            return false;
        backpressure();
        backend()->acknowledgeLogin();
        state_ = State::kConfiguration;
        readDeadline_ = backendReadDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
        backendWriteDeadline_ =
            std::min(backendWriteDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
        return true;
    }
    if (state_ == State::kConfiguration || state_ == State::kPlay) {
        if (!backend() || id < 0)
            return false;
        const auto settingsId = state_ == State::kConfiguration ? 0x00 : 0x0e;
        if (id == settingsId && !rememberSettings(raw))
            return false;
        const auto cookieResponseId = state_ == State::kConfiguration ? 0x01 : 0x15;
        if (id == cookieResponseId) {
            auto response = protocol::cookie::ResponsePacket::read(frame_);
            if (!response || frame_.readableBytes() != 0)
                return false;
            const auto revision = pendingCookies_.consume(response->key);
            if (!revision)
                return false;
            if (cookieCache_.lookup(response->key).storeRevision == *revision &&
                !cookieCache_.remember(
                    response->key, response->payload ? std::optional{std::span<const std::uint8_t>{*response->payload}}
                                                     : std::nullopt))
                return false;
        }
        bool enterPlay = false;
        bool enterConfiguration = false;
        if (state_ == State::kPlay && id == kConfigurationAcknowledgedId) {
            if (!startingConfiguration_ || frame_.readableBytes() != 0 || output_.readableBytes() != 0)
                return false;
            startingConfiguration_ = false;
            finishingConfiguration_ = false;
            state_ = State::kConfiguration;
            enterConfiguration = true;
        } else if (state_ == State::kConfiguration && id == protocol::configuration::kFinishConfigurationId) {
            if (!finishingConfiguration_ || frame_.readableBytes() != 0 || output_.readableBytes() != 0)
                return false;
            enterPlay = true;
        }
        if (!backend()->queue(raw))
            return false;
        if (enterConfiguration && clientSettings_.readableBytes() != 0 && !backend()->queue(clientSettings_.readable()))
            return false;
        if (enterPlay) {
            if (!activeBackend_)
                activeBackend_ = std::move(candidateBackend_);
            state_ = State::kPlay;
        }
        backpressure();
        if (backendWriteDeadline_ == Clock::time_point::max())
            backendWriteDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs};
        readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
        return true;
    }
    if (state_ == State::Handshake) {
        if (id != protocol::handshake::HandshakePacket::kId)
            return false;
        const auto hello = protocol::handshake::HandshakePacket::read(frame_);
        if (!hello || frame_.readableBytes() != 0 || hello->hostName.empty() ||
            hello->hostName.find('\0') != std::string::npos)
            return false;
        if (hello->intent == protocol::handshake::ClientIntent::Status) {
            state_ = State::StatusRequest;
        } else if ((hello->intent == protocol::handshake::ClientIntent::Login ||
                    (acceptTransfers_ && hello->intent == protocol::handshake::ClientIntent::Transfer)) &&
                   hello->protocolVersion == buildInfo::kProtocolVersion) {
            state_ = State::LoginStart;
            virtualHost_ = hello->hostName;
            virtualPort_ = hello->port;
            targets_ = routes_.select(virtualHost_);
            loginDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.loginTimeoutMs};
            readDeadline_ = loginDeadline_;
            return true;
        } else {
            return false;
        }
    } else if (state_ == State::LoginStart) {
        if (id != protocol::login::LoginStartPacket::kId)
            return false;
        auto start = protocol::login::LoginStartPacket::read(frame_);
        if (!start || frame_.readableBytes() != 0 || !wire::auth::validLoginName(start->name))
            return false;
        name_ = std::move(start->name);
        if (!online_) {
            profile_ = wire::auth::SessionProfile{wire::common::Uuid::offlinePlayer(name_), name_, {}};
            return route();
        }
        if (settings_.sessionBufferBytes < kBufferBytes + kAuthenticationBytes || publicKey_.empty() ||
            !wire::auth::randomBytes(challenge_))
            return false;
        protocol::login::HelloPacket hello{.serverId = "",
                                           .publicKey = {publicKey_.begin(), publicKey_.end()},
                                           .challenge = {challenge_.begin(), challenge_.end()},
                                           .shouldAuthenticate = true};
        frame_.clear();
        protocol::varInt::write(frame_, protocol::login::HelloPacket::kId);
        hello.write(frame_);
        output_.discardReadBytes();
        if (frame_.readableBytes() + 3 > kBufferCapacity - output_.writerIndex())
            return false;
        if (output_.readableBytes() == 0)
            writeDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs};
        if (!protocol::packetFraming::writeUncompressed(output_, frame_.readable()))
            return false;
        state_ = State::Key;
        return true;
    } else if (state_ == State::Key) {
        if (id != protocol::login::KeyPacket::kId)
            return false;
        auto key = protocol::login::KeyPacket::read(frame_);
        if (!key || frame_.readableBytes() != 0 || key->encryptedSecret.size() != 128 ||
            key->encryptedChallenge.size() != 128 || output_.readableBytes() != 0)
            return false;
        wire::auth::OnlineLoginRequest request{name_,
                                               checkAddress_ ? connection_.remoteAddress() : std::string{},
                                               challenge_,
                                               std::move(key->encryptedSecret),
                                               std::move(key->encryptedChallenge),
                                               loginDeadline_};
        profileLocal_ = localBuffers_.acquire(kAuthenticationBytes);
        if (!profileLocal_)
            return false;
        authentication_ = authenticate_(std::move(request));
        if (!authentication_)
            return false;
        state_ = State::Authenticating;
        return true;
    } else if (state_ == State::StatusRequest) {
        if (id != protocol::status::kStatusRequestId || frame_.readableBytes() != 0 || !queue(status_))
            return false;
        state_ = State::Ping;
    } else {
        if (id != protocol::status::PingPacket::kId)
            return false;
        const auto ping = protocol::status::PingPacket::read(frame_);
        if (!ping || frame_.readableBytes() != 0)
            return false;
        frame_.clear();
        protocol::varInt::write(frame_, protocol::status::PingPacket::kId);
        ping->write(frame_);
        // An opaque eight-byte pong, framed without allocating a staging buffer.
        std::array<std::uint8_t, 10> pong{};
        pong[0] = 9;
        std::ranges::copy(frame_.readable(), pong.begin() + 1);
        closing_ = true;
        return queue(pong);
    }
    readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
    return true;
}

bool ClientSession::decode() {
    while (input_.readableBytes() != 0 && !closing_ && !frontendPaused_ && state_ != State::Authenticating &&
           state_ != State::kResolving && state_ != State::kConnecting && state_ != State::kBackendLogin) {
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
        if (!packet())
            return false;
    }
    input_.discardReadBytes();
    return true;
}

bool ClientSession::ready(const wire::io::IoReady& event) {
    if (Clock::now() >= deadline())
        return false;
    const bool waiting = state_ == State::Authenticating || state_ == State::kResolving ||
                         state_ == State::kConnecting || state_ == State::kBackendLogin;
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
    return accounted() && (!closing_ || output_.readableBytes() != 0);
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
    if (result)
        return route();
    closing_ = true;
    return queue(authFailure_);
}

bool ClientSession::timedOut(Clock::time_point now) {
    if (now < deadline())
        return true;
    if (!closing_ && (state_ == State::kResolving || state_ == State::kConnecting) && now >= connectDeadline_ &&
        now < loginDeadline_ && now < readDeadline_ && now < writeDeadline_ && now < backendWriteDeadline_ &&
        now < backendReadDeadline_)
        return route();
    return false;
}

bool ClientSession::route() {
    attempts_.clear();
    dial_.reset();
    resolutionStorage_.reset();
    resolutionLocal_.reset();
    if (resolution_)
        resolution_->request_stop();
    resolution_.reset();
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
    if (nextTarget_ == targets_.size()) {
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
    auto attempt = attempts_.begin(targets_[nextTarget_++]);
    if (!attempt)
        return false;
    resolution_ = resolve_(*attempt, connectDeadline_);
    if (!resolution_)
        return false;
    state_ = State::kResolving;
    return true;
}

bool ClientSession::resolved(BackendAttempt attempt, std::vector<wire::io::TcpAddress> addresses,
                             wire::io::ResourceBudget::Reservation storage) {
    // A canceled lookup can finish after the same backend has been selected again.
    if (state_ != State::kResolving || !attempts_.matches(attempt))
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
    dial_ = std::make_unique<BackendDial>(std::move(addresses), connectDeadline_);
    state_ = State::kConnecting;
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
    resolutionStorage_.reset();
    resolutionLocal_.reset();
    candidateBackend_ = std::make_unique<BackendLogin>(std::move(*connected), memory_);
    if (!profile_ || !candidateBackend_->start(backends_[targets_[nextTarget_ - 1]], *profile_, virtualHost_,
                                               virtualPort_, connection_.remoteAddress())) {
        closeBackends();
        closing_ = true;
        return queue(unavailable_);
    }
    state_ = State::kBackendLogin;
    backendWriteDeadline_ =
        std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    return true;
}

BackendLogin* ClientSession::backend() const {
    // Frontend traffic remains attached to the active backend during candidate preparation.
    return activeBackend_ ? activeBackend_.get() : candidateBackend_.get();
}

void ClientSession::closeBackends() {
    candidateBackend_.reset();
    activeBackend_.reset();
}

std::int64_t ClientSession::backendHandle() const {
    return dial_ ? dial_->handle() : (backend() ? backend()->handle() : -1);
}

wire::io::IoInterest ClientSession::backendInterest() const {
    if (backend() && (state_ == State::kLoginAck || state_ == State::kCookieResponse || startingConfiguration_))
        return {false, backend()->hasOutput()};
    return backend() ? backend()->interest(!backendPaused_) : wire::io::IoInterest{false, true};
}

void ClientSession::backpressure() {
    if (backend() && backend()->pendingBytes() >= settings_.highWaterBytes)
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
            backendReadDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
    }
}

bool ClientSession::bufferedPacket() const {
    if (state_ != State::kConfiguration && state_ != State::kPlay)
        return false;
    auto hint = wire::protocol::packetFraming::inspectFrame(
        input_.readable(), compression_.has_value(), wire::protocol::packetFraming::kMaxUncompressedSize, compression_);
    return !hint || (*hint && (**hint).complete);
}

bool ClientSession::backendSuspended() const { return backendPaused_ && backend() && !backend()->hasOutput(); }
} // namespace basalt::proxy::runtime::detail
