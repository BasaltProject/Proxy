#pragma once

#include <basalt/wire/auth/SignatureValidator.hpp>
#include <basalt/wire/chat/SecureChat.hpp>
#include <basalt/wire/common/Uuid.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime::detail {

/// What the relay does with a serverbound chat packet so the backend's chat state stays consistent.
struct ChatVerdict {
    enum class Kind : std::uint8_t { kForward, kDrop, kKick };

    Kind kind = Kind::kForward;
    /// A message key from messages.toml for a kick, or a vanilla translation key for a notice.
    std::string key;
    bool notify = false;
    /// The window offset the backend must still see when the packet itself is dropped.
    std::int32_t ackOffset = 0;
    /// The packet to relay instead, id included.
    std::optional<std::vector<std::uint8_t>> replacement;
};

[[nodiscard]] bool isChatPacket(std::int32_t id) noexcept;

/// True when the packet carries a signature a backend would check (a chat message or command argument).
[[nodiscard]] bool carriesSignature(std::int32_t id, std::span<const std::uint8_t> body);

/// A plugin cancelled the packet: allowed while nothing in it is signed, otherwise the chain would break.
[[nodiscard]] ChatVerdict denyChat(std::int32_t id, std::span<const std::uint8_t> body);

/// A plugin changed the text: only an unsigned packet can be rebuilt, keeping its window update.
[[nodiscard]] ChatVerdict rewriteChat(std::int32_t id, std::span<const std::uint8_t> body, std::string_view text);

/// A plugin replaced the bytes of a chat packet; the window update must be the one the client sent.
[[nodiscard]] ChatVerdict checkReplacedChat(std::int32_t id, std::span<const std::uint8_t> original,
                                            std::span<const std::uint8_t> replaced);

/// The proxy's copy of a client's secure chat state, built from what both sides send through it.
class SecureChatGuard {
public:
    SecureChatGuard(wire::common::Uuid profileId, std::shared_ptr<const wire::auth::ServicesKeySet> services);

    [[nodiscard]] ChatVerdict serverbound(std::int32_t id, std::span<const std::uint8_t> body, std::int64_t now);

    /// False when the backend left too many signed messages unacknowledged.
    [[nodiscard]] bool clientbound(std::int32_t id, std::span<const std::uint8_t> body);

    [[nodiscard]] bool hasSession() const noexcept { return chat_.hasSession(); }

private:
    std::shared_ptr<const wire::auth::ServicesKeySet> services_;
    wire::chat::SecureChat chat_;
};

} // namespace basalt::proxy::runtime::detail
