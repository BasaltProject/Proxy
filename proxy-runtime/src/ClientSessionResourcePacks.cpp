#include "BackendLogin.hpp"
#include "ClientSession.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/text/ComponentNbt.hpp>

#include <algorithm>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::int32_t kPackSuccessfullyLoaded = 0;
constexpr std::int32_t kPackAccepted = 3;
constexpr std::int32_t kPackDownloaded = 4;
constexpr std::int32_t kConfigPackPopId = 0x08;
constexpr std::int32_t kPlayPackPopId = 0x51;
constexpr std::int32_t kPlayPackPushId = 0x52;
constexpr std::size_t kMaxPluginPacks = 16;
constexpr std::size_t kMaxPackHash = 40;
constexpr std::size_t kMaxPackUrl = 32767;

void replaceUuid(wire::io::ByteBuffer& packet, std::size_t offset, wire::common::Uuid id) {
    auto destination = packet.mutableSpan(offset).first(16);
    std::size_t at = 0;
    for (const auto half : {id.high(), id.low()})
        for (int shift = 56; shift >= 0; shift -= 8)
            destination[at++] = static_cast<std::uint8_t>(half >> shift);
}
} // namespace

bool ClientSession::backendResourcePack(bool push) {
    const auto offset = frame_.readerIndex();
    if (push) {
        const auto original = wire::protocol::readUuid(frame_);
        if (frame_.failed() || frame_.readableBytes() < 4)
            return false;
        const auto published = resourcePacks_.publish(original, backend()->attempt());
        if (!published)
            return false;
        replaceUuid(frame_, offset, *published);
    } else {
        const auto present = frame_.readByte();
        if (frame_.failed() || present > 1)
            return false;
        if (present != 0) {
            const auto original = wire::protocol::readUuid(frame_);
            if (frame_.failed() || frame_.readableBytes() != 0)
                return false;
            const auto published = resourcePacks_.remove(original, backend()->attempt());
            if (!published)
                return true;
            replaceUuid(frame_, offset + 1, *published);
        } else {
            if (frame_.readableBytes() != 0)
                return false;
            if (!pluginPacks_.empty()) {
                if (!popBackendPacks(state_ == State::kConfiguration))
                    return false;
                backend()->expectReadUntil(Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs});
                backpressure();
                return true;
            }
            resourcePacks_.clear();
        }
    }
    if (!queuePacket(frame_.mutableSpan(0)))
        return false;
    backend()->expectReadUntil(Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs});
    backpressure();
    return true;
}

bool ClientSession::resourcePackResponse() {
    const auto offset = frame_.readerIndex();
    const auto publicId = wire::protocol::readUuid(frame_);
    const auto action = wire::protocol::varInt::read(frame_);
    if (frame_.failed() || frame_.readableBytes() != 0 || action < 0 || action > 7)
        return false;
    if (const auto mine = std::ranges::find(pluginPacks_, publicId); mine != pluginPacks_.end()) {
        if (action != kPackSuccessfullyLoaded && action != kPackAccepted && action != kPackDownloaded)
            pluginPacks_.erase(mine);
        if (packStatusSink_ && profile_)
            packStatusSink_(profile_->name, publicId, action);
        readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
        return true;
    }
    const auto origin = resourcePacks_.origin(publicId);
    if (!origin || !backend() || recovering_ || origin->backend != backend()->attempt())
        return true;
    replaceUuid(frame_, offset, origin->id);
    if (!toBackend(*backend(), frame_.mutableSpan(0)))
        return false;
    if (action != kPackSuccessfullyLoaded && action != kPackAccepted && action != kPackDownloaded)
        (void)resourcePacks_.remove(origin->id, origin->backend);
    backend()->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    readDeadline_ = Clock::now() + std::chrono::milliseconds{settings_.readTimeoutMs};
    backpressure();
    return true;
}

bool ClientSession::popPack(bool configuring, wire::common::Uuid id) {
    wire::io::ByteBuffer out;
    wire::protocol::varInt::write(out, configuring ? kConfigPackPopId : kPlayPackPopId);
    wire::protocol::writeBoolean(out, true);
    wire::protocol::writeUuid(out, id);
    return queuePacket(out.readable());
}

bool ClientSession::popBackendPacks(bool configuring) {
    for (const auto id : resourcePacks_.publicIds())
        if (!popPack(configuring, id))
            return false;
    resourcePacks_.clear();
    return true;
}

bool ClientSession::applyResourcePack(const HudResourcePack& pack) {
    if (pack.op == HudResourcePack::Op::kClear) {
        for (const auto id : pluginPacks_)
            if (!popPack(false, id))
                return false;
        pluginPacks_.clear();
        return true;
    }
    const auto known = std::ranges::find(pluginPacks_, pack.id);
    if (pack.op == HudResourcePack::Op::kPop) {
        if (known == pluginPacks_.end())
            return false;
        pluginPacks_.erase(known);
        return popPack(false, pack.id);
    }
    if (pack.id.isNil() || ResourcePacks::published(pack.id) || pack.url.empty() || pack.url.size() > kMaxPackUrl ||
        pack.hash.size() > kMaxPackHash || (known == pluginPacks_.end() && pluginPacks_.size() >= kMaxPluginPacks))
        return false;
    wire::io::ByteBuffer out;
    wire::protocol::varInt::write(out, kPlayPackPushId);
    wire::protocol::writeUuid(out, pack.id);
    wire::protocol::writeString(out, pack.url);
    wire::protocol::writeString(out, pack.hash);
    wire::protocol::writeBoolean(out, pack.required);
    wire::protocol::writeBoolean(out, pack.prompt.has_value());
    if (pack.prompt)
        wire::protocol::writeAnonymousNbt(out, wire::text::componentNbt::serialize(*pack.prompt));
    if (!queuePacket(out.readable()))
        return false;
    if (known == pluginPacks_.end())
        pluginPacks_.push_back(pack.id);
    return true;
}

} // namespace basalt::proxy::runtime::detail
