#include "BackendLogin.hpp"
#include "ClientSession.hpp"
#include "RelayCounters.hpp"
#include "RouteVersions.hpp"

#include <basalt/wire/multiversion/ProtocolVersions.hpp>
#include <basalt/wire/multiversion/java/LoginCodec.hpp>
#include <basalt/wire/nbt/NbtIO.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/configuration/ConfigurationPackets.hpp>

namespace basalt::proxy::runtime::detail {
namespace {

constexpr std::int32_t kRegistryDataId = 5;
constexpr std::int32_t kFeatureFlagsId = 7;
constexpr std::int32_t kFinishConfiguration764 = 2;
constexpr std::int32_t kPlaySettingsId = 0x0e;
constexpr std::int32_t kConfigSettingsId = 0x00;

std::int32_t backendLoginId(std::int32_t protocol) {
    switch (protocol) {
    case 761:
        return 36;
    case 760:
        return 37;
    case 759:
        return 35;
    case 758:
    case 757:
    case 756:
    case 755:
        return 38;
    case 754:
        return 36;
    case 340:
        return 35;
    default:
        return 40;
    }
}

} // namespace

bool ClientSession::bridges(std::int32_t backendProtocol) const {
    return upgrades(backendProtocol) && backendProtocol < wire::multiversion::protocolVersions::kV1_20_2;
}

bool ClientSession::bridgeBackendLogin(std::span<const std::uint8_t> frame) {
    namespace protocol = wire::protocol;
    wire::io::ByteBuffer view;
    view.writeBytes(frame);
    if (protocol::varInt::read(view) != backendLoginId(backend()->protocol()))
        return false;
    bridgeLogin_.assign(frame.begin(), frame.end());
    (void)view.readInt();
    (void)view.readByte();
    (void)view.readByte();
    (void)view.readByte();
    const auto worlds = protocol::readCollectionSize(view, 1);
    for (std::int32_t index = 0; index < worlds && !view.failed(); ++index)
        (void)protocol::readString(view);
    if (view.failed())
        return false;
    const auto codec = view.readable();
    if (codec.empty() || codec[0] != 0x0a || codec.size() < 3)
        return false;
    const std::size_t nameLength = (static_cast<std::size_t>(codec[1]) << 8) | codec[2];
    if (codec.size() < 3 + nameLength)
        return false;
    std::vector<std::uint8_t> window{codec.begin() + 2 + static_cast<std::ptrdiff_t>(nameLength), codec.end()};
    window[0] = 0x0a;
    auto parsed = wire::nbt::nbtIO::readAnonymous(window);
    if (!parsed)
        return false;
    if (auto* registries = parsed->tag.get<wire::nbt::NbtCompound>())
        wire::multiversion::java::completeLoginCodec(backend()->protocol(), *registries);
    wire::io::ByteBuffer registry;
    registry.writeBytes(wire::nbt::nbtIO::writeAnonymous(parsed->tag));
    wire::io::ByteBuffer flags;
    protocol::varInt::write(flags, 1);
    protocol::writeString(flags, "minecraft:vanilla");
    const auto send = [&](std::int32_t id, std::span<const std::uint8_t> body) {
        wire::io::ByteBuffer whole;
        protocol::varInt::write(whole, id);
        whole.writeBytes(body);
        return upgradeFrame(whole.readable(), true);
    };
    if (!send(kFeatureFlagsId, flags.readable()))
        return false;
    if (!send(kRegistryDataId, registry.readable()) || !send(kFinishConfiguration764, std::span<const std::uint8_t>{}))
        return false;
    bridgeWait_ = false;
    bridgeHold_ = true;
    return true;
}

bool ClientSession::bridgedConfiguration(std::int32_t id, std::span<const std::uint8_t> raw) {
    namespace protocol = wire::protocol;
    if (id == kConfigSettingsId)
        return rememberSettings(raw);
    if (id != protocol::configuration::kFinishConfigurationId)
        return true;
    if (!finishingConfiguration_ || frame_.readableBytes() != 0)
        return false;
    pendingKeepAlives_.clear();
    pendingPings_.clear();
    if (!activeBackend_) {
        activeBackend_ = std::move(candidateBackend_);
        relayCounters().logins.fetch_add(1, std::memory_order_relaxed);
    }
    finishingConfiguration_ = false;
    state_ = State::kPlay;
    bridgeHold_ = false;
    registerBungeeChannels();
    std::vector<std::uint8_t> login;
    login.swap(bridgeLogin_);
    if (!upgradeFrame(login))
        return false;
    if (clientSettings_.readableBytes() > 1) {
        wire::io::ByteBuffer settings;
        protocol::varInt::write(settings, kPlaySettingsId);
        const auto stored = clientSettings_.readable();
        settings.writeBytes(stored.subspan(1));
        if (!toBackend(*backend(), settings.readable()))
            return false;
    }
    backpressure();
    backend()->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    backend()->expectReadUntil(Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs});
    readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
    return true;
}

} // namespace basalt::proxy::runtime::detail
