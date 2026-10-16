#pragma once

#include <basalt/wire/io/ByteBuffer.hpp>
#include <basalt/wire/multiversion/java/JavaTranslators.hpp>
#include <basalt/wire/protocol/ProtocolState.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace basalt::proxy::runtime::detail {

/// Renumbers packet ids between the session's native numbering and a client and backend that share an older version.
class IdRenumbering {
public:
    enum class Flow { kBackendToSession, kSessionToClient, kClientToSession, kSessionToBackend };
    enum class Outcome { kRenumbered, kDropped, kMalformed, kLimit };

    explicit IdRenumbering(std::unique_ptr<wire::multiversion::java::JavaChain> chain) : chain_(std::move(chain)) {}

    [[nodiscard]] static bool applies(wire::protocol::ProtocolState state) noexcept {
        return state == wire::protocol::ProtocolState::Configuration || state == wire::protocol::ProtocolState::Play;
    }

    /// Writes the frame with its id rewritten into `out`; bodies are never touched.
    [[nodiscard]] Outcome apply(Flow flow, wire::protocol::ProtocolState state, std::span<const std::uint8_t> frame,
                                wire::io::ByteBuffer& out) const;

    /// The id the backend uses for a native serverbound packet, or a negative value when it has none.
    [[nodiscard]] std::int32_t toBackendId(wire::protocol::ProtocolState state, std::int32_t id) const {
        return chain_->chain().serverboundIdFromNative(state, id);
    }

private:
    std::unique_ptr<wire::multiversion::java::JavaChain> chain_;
};

} // namespace basalt::proxy::runtime::detail
