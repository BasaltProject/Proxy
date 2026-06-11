#include "BackendDial.hpp"
#include "BackendLogin.hpp"
#include "ClientSession.hpp"
#include "RelayCounters.hpp"

#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>
#include <basalt/wire/protocol/play/BorderPackets.hpp>
#include <basalt/wire/protocol/play/CommandPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/protocol/play/UiPackets.hpp>

#include <algorithm>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr int kMaxTapDepth = 2;
constexpr std::int32_t kConfigurationAcknowledgedPlayId = 0x10;
constexpr std::int32_t kStartConfigurationPlayId = 0x78;

[[nodiscard]] bool contains(std::span<const std::int32_t> ids, std::int32_t id) {
    return std::ranges::find(ids, id) != ids.end();
}
} // namespace

class ClientSession::SessionEndpoint final : public wire::plugins::PacketEndpoint {
public:
    explicit SessionEndpoint(ClientSession& session) : session_(&session) {}

    void detach() noexcept { session_ = nullptr; }

    bool sendClientbound(std::int32_t id, std::span<const std::uint8_t> body, bool intercept) override {
        return session_ != nullptr && session_->injectPacket(false, id, body, intercept);
    }

    bool receiveServerbound(std::int32_t id, std::span<const std::uint8_t> body, bool intercept) override {
        return session_ != nullptr && session_->injectPacket(true, id, body, intercept);
    }

    void close() override {
        if (session_ != nullptr)
            session_->closing_ = true;
    }

    [[nodiscard]] std::uint64_t connectionId() const override { return session_ ? session_->connectionId_ : 0; }

    [[nodiscard]] std::string address() const override {
        return session_ ? session_->connection_.remoteAddress() : std::string{};
    }

    [[nodiscard]] std::uint16_t port() const override { return session_ ? session_->connection_.remotePort() : 0; }

private:
    ClientSession* session_;
};

void ClientSession::attachPackets(std::shared_ptr<wire::plugins::PluginPacketBus> bus, std::uint64_t connection,
                                  std::size_t maxBytes) {
    packets_ = std::move(bus);
    connectionId_ = connection;
    tapMaxBytes_ = maxBytes;
}

ClientSession::~ClientSession() {
    if (endpoint_)
        endpoint_->detach();
    if (packets_ && connectionId_ != 0)
        packets_->connectionClosed(connectionId_);
    closeBackends();
    if (identityClaimed_)
        identities_.release(profile_->id);
    if (authentication_)
        authentication_->request_stop();
}

std::optional<wire::plugins::PacketPhase> ClientSession::tapPhase() const {
    using wire::plugins::PacketPhase;
    switch (state_) {
    case State::LoginStart:
    case State::Key:
    case State::Authenticating:
    case State::kBackendLogin:
    case State::kCookieResponse:
    case State::kLoginAck:
        return PacketPhase::Login;
    case State::kConfiguration:
        return PacketPhase::Configuration;
    case State::kPlay:
        return PacketPhase::Play;
    default:
        return std::nullopt;
    }
}

// Packets whose bytes the proxy itself answers or acts on are shown but cannot be cancelled or edited.
bool ClientSession::tapEditable(bool serverbound, std::int32_t id) const {
    if (legacyRaw_ || state_ == State::LoginStart || state_ == State::Key || state_ == State::Authenticating ||
        state_ == State::kBackendLogin || state_ == State::kCookieResponse || state_ == State::kLoginAck)
        return false;
    namespace configuration = wire::protocol::configuration;
    namespace play = wire::protocol::play;
    if (state_ == State::kConfiguration) {
        static constexpr std::int32_t kToBackend[] = {0x01, configuration::kFinishConfigurationId,
                                                      configuration::KeepAlivePacket::kId, 0x05, 0x06};
        static constexpr std::int32_t kToClient[] = {0x00,
                                                     0x02,
                                                     0x05,
                                                     0x08,
                                                     0x09,
                                                     0x0b,
                                                     0x0c,
                                                     configuration::kFinishConfigurationId,
                                                     configuration::KeepAlivePacket::kId};
        return !contains(
            serverbound ? std::span<const std::int32_t>{kToBackend} : std::span<const std::int32_t>{kToClient}, id);
    }
    static constexpr std::int32_t kToBackend[] = {kConfigurationAcknowledgedPlayId,
                                                  play::PlayKeepAlivePacket::kServerboundId, 0x2d, 0x15, 0x32};
    static constexpr std::int32_t kToClient[] = {play::PlayKeepAlivePacket::kClientboundId,
                                                 0x3d,
                                                 0x15,
                                                 0x50,
                                                 0x51,
                                                 0x7a,
                                                 kStartConfigurationPlayId,
                                                 play::TransferPacket::kId,
                                                 play::PlayDisconnectPacket::kId};
    return !contains(serverbound ? std::span<const std::int32_t>{kToBackend} : std::span<const std::int32_t>{kToClient},
                     id);
}

ClientSession::Tapped ClientSession::tap(wire::io::ByteBuffer& source, bool serverbound, std::int32_t id,
                                         std::span<const std::uint8_t>& whole) {
    if (!packets_ || synthesizing_ || tapDepth_ >= kMaxTapDepth)
        return Tapped::kPass;
    const auto phase = tapPhase();
    if (!phase)
        return Tapped::kPass;
    const wire::plugins::PacketAddress address{*phase, serverbound, id};
    if (!packets_->listening(address))
        return Tapped::kPass;
    const auto body = source.readable();
    if (body.size() > tapMaxBytes_) {
        relayCounters().packetsOversize.fetch_add(1, std::memory_order_relaxed);
        return Tapped::kPass;
    }
    wire::plugins::PluginPacket nested;
    auto& packet = tapDepth_ == 0 ? tapped_ : nested;
    packet.address = address;
    packet.name.clear();
    packet.connection = connectionId_;
    packet.clientProtocol = clientProtocol_;
    BackendLogin* target = backend();
    packet.backendProtocol = target ? target->protocol() : 0;
    packet.translated = translation_ != nullptr || (target && upgrades(target->protocol()));
    if (packet.playerName.empty() && profile_) {
        packet.playerName = profile_->name;
        packet.playerUuid = profile_->id.toString();
    }
    if (target && target->attempt().target < backends_.size())
        packet.backend.assign(backends_[target->attempt().target].name);
    else
        packet.backend.clear();
    packet.editable = tapEditable(serverbound, id);
    packet.bytes.assign(body.begin(), body.end());
    packet.cancelled = false;
    packet.replacement.reset();
    if (!endpoint_)
        endpoint_ = std::make_shared<SessionEndpoint>(*this);
    packet.endpoint = endpoint_;
    ++tapDepth_;
    const bool delivered = packets_->fire(packet);
    --tapDepth_;
    if (!delivered)
        return Tapped::kDrop;
    if (!packet.replacement || packet.replacement->size() > tapMaxBytes_)
        return Tapped::kPass;
    source.clear();
    if (!source.tryReserve(packet.replacement->size() + 8))
        return Tapped::kFailed;
    wire::protocol::varInt::write(source, id);
    source.writeBytes(*packet.replacement);
    whole = source.readable();
    (void)wire::protocol::varInt::read(source);
    return Tapped::kChanged;
}

bool ClientSession::injectPacket(bool serverbound, std::int32_t id, std::span<const std::uint8_t> body,
                                 bool intercept) {
    if (closing_ || (state_ != State::kConfiguration && state_ != State::kPlay) || !backend() || recovering_)
        return false;
    if (id < 0 || body.size() > tapMaxBytes_)
        return false;
    wire::io::ByteBuffer packet;
    wire::protocol::varInt::write(packet, id);
    packet.writeBytes(body);
    auto whole = packet.readable();
    (void)wire::protocol::varInt::read(packet);
    if (intercept) {
        switch (tap(packet, serverbound, id, whole)) {
        case Tapped::kDrop:
            return true;
        case Tapped::kFailed:
            return false;
        default:
            break;
        }
    }
    if (!serverbound)
        return queuePacket(whole);
    if (!toBackend(*backend(), whole))
        return false;
    backpressure();
    backend()->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    return true;
}

bool ClientSession::pluginPacket(bool serverbound, std::int32_t phase, std::int32_t id,
                                 std::span<const std::uint8_t> body) {
    const auto current = tapPhase();
    if (!current || static_cast<std::int32_t>(*current) != phase)
        return false;
    return injectPacket(serverbound, id, body, false);
}

} // namespace basalt::proxy::runtime::detail
