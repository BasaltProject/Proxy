#include "JavaLink.hpp"

#include "../NativeForwarding.hpp"

#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/io/DnsResolver.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>

#include <memory_resource>

namespace basalt::proxy::runtime::detail {
namespace {

namespace protocol = wire::protocol;

constexpr std::size_t kMaxPacketsPerPump = 256;
constexpr std::int32_t kLoginDisconnect = 0;
constexpr std::int32_t kLoginFinished = 2;
constexpr std::int32_t kLoginPluginRequest = 4;
constexpr std::int32_t kConfigurationPing = 0x05;
constexpr std::int32_t kPlayPing = 0x3d;
constexpr std::int32_t kPlayPong = 0x2d;

[[nodiscard]] std::string textOf(wire::io::ByteBuffer& body) {
    auto reason = protocol::readString(body);
    return body.failed() ? std::string{"the backend closed the connection"} : reason;
}

} // namespace

JavaLink::JavaLink(config::BackendConfig backend, wire::auth::SessionProfile profile, std::string clientAddress,
                   std::uint16_t clientPort, Events events, const NativeForwarding* native, std::string originId)
    : backend_(std::move(backend)), profile_(std::move(profile)), clientAddress_(std::move(clientAddress)),
      clientPort_(clientPort), events_(std::move(events)), native_(native), originId_(std::move(originId)) {}

bool JavaLink::begin(std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    auto addresses = wire::io::resolveDns(backend_.host, backend_.port, wire::io::DnsOptions{.deadline = deadline});
    if (!addresses || addresses->empty())
        return false;
    dial_ = std::make_unique<BackendDial>(std::move(*addresses), deadline);
    static_cast<void>(dial_->advance());
    state_ = State::kConnecting;
    return true;
}

void JavaLink::close(const std::string& reason) {
    if (state_ == State::kClosed)
        return;
    state_ = State::kClosed;
    if (login_)
        login_->retire();
    if (events_.closed)
        events_.closed(reason);
}

void JavaLink::pump() {
    if (state_ == State::kConnecting) {
        auto progress = dial_->advance();
        if (!progress) {
            close("could not connect to the backend");
            return;
        }
        if (*progress == DialState::kConnecting)
            return;
        auto connected = dial_->take();
        if (!connected) {
            close("could not connect to the backend");
            return;
        }
        dial_.reset();
        login_ = std::make_unique<BackendLogin>(std::move(*connected), *std::pmr::new_delete_resource(),
                                                BackendAttempt{1, 0});
        login_->useOrigin(1, originId_);
        if (!login_->start(backend_, profile_, backend_.host, backend_.port, clientAddress_, clientPort_, native_)) {
            close("the backend login could not start");
            return;
        }
        state_ = State::kLogin;
    }
    for (std::size_t i = 0;
         i < kMaxPacketsPerPump && state_ != State::kClosed && state_ != State::kIdle && state_ != State::kConnecting;
         ++i) {
        auto packet = login_->ready(wire::io::IoReady{.handle = login_->handle(), .readable = true, .writable = true});
        if (!packet) {
            close("the backend connection ended");
            return;
        }
        if (packet->empty())
            return;
        handle(*packet);
    }
}

void JavaLink::sendFrame(std::int32_t id, std::span<const std::uint8_t> body) {
    scratch_.clear();
    protocol::varInt::write(scratch_, id);
    scratch_.writeBytes(body);
    if (!login_ || !login_->queue(scratch_.readable(), 8 * 1024 * 1024))
        close("the backend connection is full");
}

void JavaLink::send(std::int32_t id, std::span<const std::uint8_t> body) {
    if (state_ == State::kPlay)
        sendFrame(id, body);
}

void JavaLink::handle(std::span<const std::uint8_t> packet) {
    wire::io::ByteBuffer body;
    body.writeBytes(packet);
    const auto id = protocol::varInt::read(body);
    if (body.failed()) {
        close("the backend sent a malformed packet");
        return;
    }
    if (state_ == State::kLogin)
        handleLogin(id, body);
    else if (state_ == State::kConfiguration)
        handleConfiguration(id, body);
    else if (state_ == State::kPlay)
        handlePlay(id, body);
}

void JavaLink::handleLogin(std::int32_t id, wire::io::ByteBuffer& body) {
    if (id == kLoginPluginRequest) {
        if (!login_->answerPluginQuery(body))
            close("the backend asked for something this proxy cannot give");
        return;
    }
    if (id == kLoginDisconnect) {
        close("the backend refused the login");
        return;
    }
    if (id != kLoginFinished) {
        close("the backend sent an unexpected login packet");
        return;
    }
    if (!login_->acceptLogin(body)) {
        close("the backend login did not match the player");
        return;
    }
    sendFrame(protocol::login::kLoginAcknowledgedId, {});
    if (!login_->acknowledgeLogin()) {
        close("the backend login could not be acknowledged");
        return;
    }
    state_ = State::kConfiguration;
    scratch_.clear();
    protocol::configuration::ClientInformationPacket{.locale = "en_us",
                                                     .viewDistance = 8,
                                                     .chatFlags = 0,
                                                     .chatColors = true,
                                                     .skinParts = 0x7f,
                                                     .mainHand = 1,
                                                     .enableTextFiltering = false,
                                                     .enableServerListing = true,
                                                     .particleStatus = 0}
        .write(scratch_);
    const std::vector<std::uint8_t> information{scratch_.readable().begin(), scratch_.readable().end()};
    sendFrame(protocol::configuration::ClientInformationPacket::kId, information);
}

void JavaLink::handleConfiguration(std::int32_t id, wire::io::ByteBuffer& body) {
    namespace configuration = protocol::configuration;
    if (id == configuration::kFinishConfigurationId) {
        sendFrame(configuration::kFinishConfigurationId, {});
        if (!login_->finishConfiguration()) {
            close("the backend configuration could not finish");
            return;
        }
        state_ = State::kPlay;
    } else if (id == configuration::SelectKnownPacksPacket::kClientboundId) {
        scratch_.clear();
        configuration::SelectKnownPacksPacket{}.write(scratch_);
        const std::vector<std::uint8_t> none{scratch_.readable().begin(), scratch_.readable().end()};
        sendFrame(configuration::SelectKnownPacksPacket::kServerboundId, none);
    } else if (id == configuration::KeepAlivePacket::kId) {
        sendFrame(configuration::KeepAlivePacket::kId, body.readable());
    } else if (id == kConfigurationPing) {
        sendFrame(kConfigurationPing, body.readable());
    } else if (id == configuration::RegistryDataPacket::kId) {
        if (const auto registry = configuration::RegistryDataPacket::read(body); registry && events_.registry) {
            std::vector<std::string> keys;
            keys.reserve(registry->entries.size());
            for (const auto& entry : registry->entries)
                keys.push_back(entry.key);
            events_.registry(registry->registryId, std::move(keys));
        }
    } else if (id == 2) {
        close(textOf(body));
    }
}

void JavaLink::handlePlay(std::int32_t id, wire::io::ByteBuffer& body) {
    if (id == protocol::play::PlayKeepAlivePacket::kClientboundId) {
        sendFrame(protocol::play::PlayKeepAlivePacket::kServerboundId, body.readable());
        return;
    }
    if (id == kPlayPing) {
        sendFrame(kPlayPong, body.readable());
        return;
    }
    if (events_.play)
        events_.play(id, body.readable());
}

} // namespace basalt::proxy::runtime::detail
