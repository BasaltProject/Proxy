#include "StatusFrameCache.hpp"

#include <basalt/wire/io/ByteBuffer.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/VarInt.hpp>

#include <utility>

namespace basalt::proxy::runtime::detail {

std::vector<std::uint8_t> StatusFrameCache::frame(wire::protocol::status::ServerStatusPayload payload) {
    namespace protocol = wire::protocol;
    std::shared_ptr<const Entry> last;
    {
        const std::lock_guard lock{mutex_};
        last = last_;
    }
    if (last && last->payload == payload)
        return last->frame;
    wire::io::ByteBuffer body{1024}, framed{1024};
    protocol::varInt::write(body, protocol::status::StatusResponsePacket::kId);
    protocol::status::StatusResponsePacket{payload}.write(body);
    if (!protocol::packetFraming::writeUncompressed(framed, body.readable()))
        return {};
    auto fresh = std::make_shared<const Entry>(Entry{std::move(payload), framed.toVector()});
    const std::lock_guard lock{mutex_};
    last_ = fresh;
    return fresh->frame;
}

} // namespace basalt::proxy::runtime::detail
