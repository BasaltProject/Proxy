#include "IdRenumbering.hpp"

#include <basalt/wire/protocol/VarInt.hpp>

namespace basalt::proxy::runtime::detail {

IdRenumbering::Outcome IdRenumbering::apply(Flow flow, wire::protocol::ProtocolState state,
                                            std::span<const std::uint8_t> frame, wire::io::ByteBuffer& out) const {
    std::size_t idBytes = 0;
    std::uint32_t id = 0;
    bool complete = false;
    while (!complete && idBytes < frame.size() &&
           idBytes < static_cast<std::size_t>(wire::protocol::varInt::kMaxBytes)) {
        const std::uint8_t byte = frame[idBytes];
        id |= static_cast<std::uint32_t>(byte & 0x7fU) << (7 * idBytes);
        ++idBytes;
        complete = (byte & 0x80U) == 0;
    }
    if (!complete)
        return Outcome::kMalformed;
    const auto& chain = chain_->chain();
    const auto source = static_cast<std::int32_t>(id);
    std::int32_t mapped = 0;
    switch (flow) {
    case Flow::kBackendToSession:
        mapped = chain.clientboundIdToNative(state, source);
        break;
    case Flow::kSessionToClient:
        mapped = chain.clientboundIdFromNative(state, source);
        break;
    case Flow::kClientToSession:
        mapped = chain.serverboundIdToNative(state, source);
        break;
    case Flow::kSessionToBackend:
        mapped = chain.serverboundIdFromNative(state, source);
        break;
    }
    if (mapped < 0)
        return Outcome::kDropped;
    out.clear();
    if (!out.tryReserve(frame.size() + 8))
        return Outcome::kLimit;
    wire::protocol::varInt::write(out, mapped);
    out.writeBytes(frame.subspan(idBytes));
    return Outcome::kRenumbered;
}

} // namespace basalt::proxy::runtime::detail
