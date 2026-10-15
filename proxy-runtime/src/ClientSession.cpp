#include "ClientSession.hpp"

#include "BackendDial.hpp"
#include "BackendLogin.hpp"
#include "BackendRoutes.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/wire/auth/SessionKeyPair.hpp>
#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>
#include <basalt/wire/protocol/handshake/HandshakePacket.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/login/LoginPluginPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/status/StatusPackets.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::int32_t kConfigurationDisconnectId = 2;
}

ClientSession::ClientSession(wire::io::TcpConnection connection, wire::io::ResourceBudget::Reservation slot,
                             wire::io::ResourceBudget::Reservation buffers, const config::RuntimeConfig& settings,
                             std::span<const std::uint8_t> status, std::span<const std::uint8_t> unavailable,
                             Clock::time_point acceptedAt, bool online, bool checkAddress,
                             std::span<const std::uint8_t> publicKey, std::span<const std::uint8_t> authFailure,
                             AuthenticationStart authenticate, const BackendRoutes& routes, ResolutionStart resolve,
                             const std::vector<config::BackendConfig>& backends)
    : slot_(std::move(slot)), buffers_(std::move(buffers)), connection_(std::move(connection)), settings_(settings),
      status_(status), unavailable_(unavailable), online_(online), checkAddress_(checkAddress), publicKey_(publicKey),
      authFailure_(authFailure), authenticate_(std::move(authenticate)), routes_(routes), resolve_(std::move(resolve)),
      backends_(backends), readDeadline_(acceptedAt + std::chrono::milliseconds{settings.handshakeTimeoutMs}) {}

ClientSession::~ClientSession() {
    if (authentication_)
        authentication_->request_stop();
    if (resolution_)
        resolution_->request_stop();
}

bool ClientSession::accounted() const {
    return input_.capacity() + frame_.capacity() + output_.capacity() + (backend_ ? backend_->allocated() : 0) <=
           buffers_.size();
}

ClientSession::Clock::time_point ClientSession::deadline() const {
    auto result = closing_ ? writeDeadline_ : std::min(readDeadline_, writeDeadline_);
    if (!closing_ && (state_ == State::kResolving || state_ == State::kConnecting))
        result = std::min(result, connectDeadline_);
    return closing_ ? result : std::min({result, backendWriteDeadline_, backendReadDeadline_});
}

wire::io::IoInterest ClientSession::interest() const {
    return {!closing_ && !paused_ && state_ != State::Authenticating && state_ != State::kResolving &&
                state_ != State::kConnecting && state_ != State::kBackendLogin,
            output_.readableBytes() != 0};
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
        paused_ = true;
    return true;
}

bool ClientSession::queuePacket(std::span<const std::uint8_t> packet) {
    output_.discardReadBytes();
    if (packet.empty() || packet.size() + 3 > kBufferCapacity - output_.writerIndex())
        return false;
    if (output_.readableBytes() == 0)
        writeDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs};
    const auto offset = output_.writerIndex();
    if (!wire::protocol::packetFraming::writeUncompressed(output_, packet))
        return false;
    if (encryptor_)
        encryptor_->encrypt(output_.mutableSpan(offset));
    if (output_.readableBytes() >= settings_.highWaterBytes)
        paused_ = true;
    return true;
}

bool ClientSession::packet() {
    namespace protocol = wire::protocol;
    const auto raw = frame_.readable();
    const auto id = protocol::varInt::read(frame_);
    if (frame_.failed())
        return false;
    if (state_ == State::kLoginAck) {
        if (id != protocol::login::kLoginAcknowledgedId || frame_.readableBytes() != 0 ||
            output_.readableBytes() != 0 || !backend_)
            return false;
        if (!backend_->queue(raw))
            return false;
        backend_->acknowledgeLogin();
        state_ = State::kConfiguration;
        readDeadline_ = backendReadDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
        backendWriteDeadline_ =
            std::min(backendWriteDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
        return true;
    }
    if (state_ == State::kConfiguration || state_ == State::kPlay) {
        if (!backend_ || id < 0)
            return false;
        if (state_ == State::kConfiguration && id == protocol::configuration::kFinishConfigurationId) {
            if (!finishingConfiguration_ || frame_.readableBytes() != 0 || output_.readableBytes() != 0)
                return false;
            state_ = State::kPlay;
        }
        if (!backend_->queue(raw))
            return false;
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
        } else if (hello->intent == protocol::handshake::ClientIntent::Login &&
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
    while (input_.readableBytes() != 0 && !closing_ && state_ != State::Authenticating && state_ != State::kResolving &&
           state_ != State::kConnecting && state_ != State::kBackendLogin) {
        // Reject the declared bootstrap size before buffering its body or running a codec.
        std::uint32_t length = 0;
        bool complete = false;
        for (std::size_t i = 0; i < std::min<std::size_t>(input_.readableBytes(), 3); ++i) {
            const auto byte = input_.peekByte(i);
            length |= static_cast<std::uint32_t>(byte & 0x7fU) << (i * 7);
            if ((byte & 0x80U) == 0) {
                complete = true;
                break;
            }
        }
        if (!complete) {
            if (input_.readableBytes() >= 3)
                return false;
            break;
        }
        if (length == 0 || length > kBufferCapacity - 3)
            return false;
        auto decoded = wire::protocol::packetFraming::tryReadFrame(input_, false, frame_);
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
    if (!closing_ && !paused_ && !waiting && (event.readable || event.closed)) {
        std::array<std::uint8_t, kBufferCapacity> chunk{};
        const auto room = kBufferCapacity - input_.writerIndex();
        if (room == 0)
            return false;
        const auto read = connection_.read(std::span{chunk}.first(room));
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
            if (output_.readableBytes() <= settings_.lowWaterBytes)
                paused_ = false;
        }
    }
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
        if (!profileStorage_->resize(retained))
            return false;
    }
    if (result)
        return route();
    closing_ = true;
    return queue(authFailure_);
}

bool ClientSession::route() {
    dial_.reset();
    resolutionStorage_.reset();
    if (resolution_)
        resolution_->request_stop();
    resolution_.reset();
    if (nextTarget_ == targets_.size()) {
        closing_ = true;
        return queue(unavailable_);
    }
    if (settings_.sessionBufferBytes <
        kBufferBytes + kResolutionBytes + (profileStorage_ ? profileStorage_->size() : 0))
        return false;
    connectDeadline_ = std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.connectTimeoutMs});
    resolution_ = resolve_(targets_[nextTarget_++], connectDeadline_);
    if (!resolution_)
        return false;
    state_ = State::kResolving;
    return true;
}

bool ClientSession::resolved(std::size_t target, std::vector<wire::io::TcpAddress> addresses,
                             wire::io::ResourceBudget::Reservation storage) {
    if (state_ != State::kResolving || nextTarget_ == 0 || targets_[nextTarget_ - 1] != target ||
        Clock::now() >= deadline())
        return false;
    resolution_.reset();
    if (addresses.empty()) {
        storage.reset();
        return route();
    }
    if (addresses.size() > 32 || addresses.capacity() > storage.size() / sizeof(wire::io::TcpAddress))
        return false;
    if (!storage.resize(addresses.capacity() * sizeof(wire::io::TcpAddress)))
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
    const auto bytes = kBufferBytes + BackendLogin::kBytes;
    if (bytes + (profileStorage_ ? profileStorage_->size() : 0) > settings_.sessionBufferBytes ||
        !buffers_.resize(bytes))
        return false;
    backend_ = std::make_unique<BackendLogin>(std::move(*connected));
    if (!profile_ || !backend_->start(backends_[targets_[nextTarget_ - 1]], *profile_, virtualHost_, virtualPort_)) {
        backend_.reset();
        (void)buffers_.resize(kBufferBytes);
        closing_ = true;
        return queue(unavailable_);
    }
    state_ = State::kBackendLogin;
    backendWriteDeadline_ =
        std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    return true;
}

bool ClientSession::backendReady(const wire::io::IoReady& event) {
    if (Clock::now() >= deadline())
        return false;
    if (state_ == State::kConnecting && dial_)
        return dial();
    if ((state_ != State::kBackendLogin && state_ != State::kLoginAck && state_ != State::kConfiguration &&
         state_ != State::kPlay) ||
        !backend_)
        return false;
    auto packet = backend_->ready(event);
    if (!packet)
        return false;
    if (!backend_->hasOutput())
        backendWriteDeadline_ = Clock::time_point::max();
    if (packet->empty())
        return true;
    if (packet->size() > kBufferCapacity - 3)
        return false;
    frame_.clear();
    frame_.writeBytes(*packet);
    const auto id = wire::protocol::varInt::read(frame_);
    if (frame_.failed())
        return false;
    if (state_ == State::kLoginAck)
        return false;
    if (state_ == State::kConfiguration || state_ == State::kPlay) {
        if (id < 0)
            return false;
        const bool terminal = (state_ == State::kConfiguration && id == kConfigurationDisconnectId) ||
                              (state_ == State::kPlay && id == wire::protocol::play::PlayDisconnectPacket::kId);
        if (terminal) {
            if (!wire::protocol::play::PlayDisconnectPacket::read(frame_) || frame_.readableBytes() != 0)
                return false;
        } else if (state_ == State::kConfiguration && id == wire::protocol::configuration::kFinishConfigurationId) {
            if (finishingConfiguration_ || frame_.readableBytes() != 0)
                return false;
            finishingConfiguration_ = true;
        } else if (state_ == State::kConfiguration && finishingConfiguration_) {
            return false;
        }
        frame_.clear();
        if (!wire::protocol::packetFraming::writeUncompressed(frame_, *packet) || !queue(frame_.readable()))
            return false;
        if (terminal) {
            closing_ = true;
            backend_.reset();
            (void)buffers_.resize(kBufferBytes);
        }
        backendReadDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
        return true;
    }
    if (id == wire::protocol::login::LoginFinishedPacket::kId) {
        auto finished = wire::protocol::login::LoginFinishedPacket::read(frame_);
        if (!finished || frame_.readableBytes() != 0 || !profile_ || finished->gameProfile.name != profile_->name ||
            (finished->gameProfile.id != profile_->id &&
             finished->gameProfile.id != wire::common::Uuid::offlinePlayer(profile_->name)))
            return false;
        // The backend's explicit offline identity must not replace the verified frontend UUID.
        finished->gameProfile.id = profile_->id;
        frame_.clear();
        wire::protocol::varInt::write(frame_, wire::protocol::login::LoginFinishedPacket::kId);
        finished->write(frame_);
        if (!queuePacket(frame_.readable()))
            return false;
        state_ = State::kLoginAck;
        return true;
    }
    if (id == wire::protocol::login::LoginPluginRequestPacket::kId) {
        auto request = wire::protocol::login::LoginPluginRequestPacket::read(frame_);
        if (!request || frame_.readableBytes() != 0)
            return false;
        frame_.clear();
        wire::protocol::varInt::write(frame_, wire::protocol::login::LoginPluginResponsePacket::kId);
        wire::protocol::login::LoginPluginResponsePacket{request->transactionId, std::nullopt}.write(frame_);
        if (!backend_->queue(frame_.readable()))
            return false;
        if (backendWriteDeadline_ == Clock::time_point::max())
            backendWriteDeadline_ =
                std::min(loginDeadline_, Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
        return true;
    }
    closing_ = true;
    if (id != wire::protocol::login::LoginDisconnectPacket::kId) {
        backend_.reset();
        (void)buffers_.resize(kBufferBytes);
        return queue(unavailable_);
    }
    const auto reason = wire::protocol::readString(frame_);
    if (frame_.failed() || frame_.readableBytes() != 0 || !wire::common::parseJson(reason))
        return false;
    frame_.clear();
    if (!wire::protocol::packetFraming::writeUncompressed(frame_, *packet))
        return false;
    backend_.reset();
    (void)buffers_.resize(kBufferBytes);
    return queue(frame_.readable());
}

std::int64_t ClientSession::backendHandle() const {
    return dial_ ? dial_->handle() : (backend_ ? backend_->handle() : -1);
}

wire::io::IoInterest ClientSession::backendInterest() const {
    if (backend_ && state_ == State::kLoginAck)
        return {false, backend_->hasOutput()};
    return backend_ ? backend_->interest() : wire::io::IoInterest{false, true};
}

} // namespace basalt::proxy::runtime::detail
