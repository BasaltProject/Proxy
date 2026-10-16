#include "BackendLogin.hpp"
#include "ClientSession.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::int32_t kPackSuccessfullyLoaded = 0;
constexpr std::int32_t kPackAccepted = 3;
constexpr std::int32_t kPackDownloaded = 4;

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

} // namespace basalt::proxy::runtime::detail
