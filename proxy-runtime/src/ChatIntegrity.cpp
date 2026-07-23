#include "ChatIntegrity.hpp"

#include <basalt/wire/chat/ChatMessage.hpp>
#include <basalt/wire/io/ByteBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/play/ChatPackets.hpp>
#include <basalt/wire/protocol/play/CommandPackets.hpp>

#include <utility>

namespace basalt::proxy::runtime::detail {
namespace {

namespace play = wire::protocol::play;

constexpr std::string_view kAltered = "chat-signed-altered";
constexpr std::string_view kValidationFailed = "chat-validation-failed";

wire::io::ByteBuffer viewOf(std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer view;
    view.writeBytes(body);
    return view;
}

template <typename Packet>
std::vector<std::uint8_t> framed(const Packet& packet) {
    wire::io::ByteBuffer out;
    wire::protocol::varInt::write(out, Packet::kId);
    packet.write(out);
    return out.toVector();
}

ChatVerdict kick(std::string_view key) {
    ChatVerdict verdict;
    verdict.kind = ChatVerdict::Kind::kKick;
    verdict.key = key;
    return verdict;
}

ChatVerdict drop(std::int32_t offset, std::string key = {}) {
    ChatVerdict verdict;
    verdict.kind = ChatVerdict::Kind::kDrop;
    verdict.ackOffset = offset;
    verdict.notify = !key.empty();
    verdict.key = std::move(key);
    return verdict;
}

} // namespace

bool isChatPacket(std::int32_t id) noexcept {
    return id == play::ChatMessagePacket::kId || id == play::SignedChatCommandPacket::kId ||
           id == play::ChatCommandPacket::kId || id == play::ChatAckPacket::kId ||
           id == play::ChatSessionUpdatePacket::kId;
}

bool carriesSignature(std::int32_t id, std::span<const std::uint8_t> body) {
    auto view = viewOf(body);
    if (id == play::ChatMessagePacket::kId) {
        const auto packet = play::ChatMessagePacket::read(view);
        return !packet || packet->signature.has_value();
    }
    if (id == play::SignedChatCommandPacket::kId) {
        const auto packet = play::SignedChatCommandPacket::read(view);
        return !packet || !packet->argumentSignatures.empty();
    }
    return false;
}

ChatVerdict denyChat(std::int32_t id, std::span<const std::uint8_t> body) {
    auto view = viewOf(body);
    if (id == play::ChatMessagePacket::kId) {
        const auto packet = play::ChatMessagePacket::read(view);
        if (!packet || packet->signature)
            return kick(kAltered);
        return drop(packet->lastSeen.offset);
    }
    if (id == play::SignedChatCommandPacket::kId) {
        const auto packet = play::SignedChatCommandPacket::read(view);
        if (!packet || !packet->argumentSignatures.empty())
            return kick(kAltered);
        return drop(packet->lastSeen.offset);
    }
    return drop(0);
}

ChatVerdict rewriteChat(std::int32_t id, std::span<const std::uint8_t> body, std::string_view text) {
    auto view = viewOf(body);
    ChatVerdict verdict;
    if (id == play::ChatMessagePacket::kId) {
        auto packet = play::ChatMessagePacket::read(view);
        if (!packet || packet->signature)
            return kick(kAltered);
        packet->message = std::string{text.substr(0, play::ChatMessagePacket::kMaxLength)};
        verdict.replacement = framed(*packet);
    } else if (id == play::SignedChatCommandPacket::kId) {
        auto packet = play::SignedChatCommandPacket::read(view);
        if (!packet || !packet->argumentSignatures.empty())
            return kick(kAltered);
        packet->command = std::string{text};
        verdict.replacement = framed(*packet);
    } else if (id == play::ChatCommandPacket::kId) {
        verdict.replacement = framed(play::ChatCommandPacket{std::string{text}});
    }
    return verdict;
}

ChatVerdict checkReplacedChat(std::int32_t id, std::span<const std::uint8_t> original,
                              std::span<const std::uint8_t> replaced) {
    auto before = viewOf(original);
    auto after = viewOf(replaced);
    if (id == play::ChatMessagePacket::kId) {
        const auto was = play::ChatMessagePacket::read(before);
        const auto now = play::ChatMessagePacket::read(after);
        if (!was || was->signature)
            return kick(kAltered);
        if (!now || now->signature || !(now->lastSeen == was->lastSeen))
            return drop(was->lastSeen.offset);
    } else if (id == play::SignedChatCommandPacket::kId) {
        const auto was = play::SignedChatCommandPacket::read(before);
        const auto now = play::SignedChatCommandPacket::read(after);
        if (!was || !was->argumentSignatures.empty())
            return kick(kAltered);
        if (!now || !now->argumentSignatures.empty() || !(now->lastSeen == was->lastSeen))
            return drop(was->lastSeen.offset);
    }
    return {};
}

SecureChatGuard::SecureChatGuard(wire::common::Uuid profileId,
                                 std::shared_ptr<const wire::auth::ServicesKeySet> services)
    : services_(std::move(services)), chat_(profileId, true, services_.get()) {}

ChatVerdict SecureChatGuard::serverbound(std::int32_t id, std::span<const std::uint8_t> body, std::int64_t now) {
    auto view = viewOf(body);
    if (id == play::ChatSessionUpdatePacket::kId) {
        const auto packet = play::ChatSessionUpdatePacket::read(view);
        if (!packet)
            return {};
        switch (chat_.updateSession(packet->session)) {
        case wire::chat::SecureChat::SessionResult::ExpiredKey:
            return kick("expired-public-key");
        case wire::chat::SecureChat::SessionResult::InvalidSignature:
            return kick("invalid-public-key-signature");
        default:
            return {};
        }
    }
    if (id == play::ChatAckPacket::kId) {
        const auto packet = play::ChatAckPacket::read(view);
        if (packet && !chat_.applyAck(packet->offset))
            return kick(kValidationFailed);
        return {};
    }
    if (id == play::SignedChatCommandPacket::kId) {
        const auto packet = play::SignedChatCommandPacket::read(view);
        if (!packet)
            return {};
        if (!chat_.applyLastSeen(packet->lastSeen))
            return kick(kValidationFailed);
        chat_.skipSigned(packet->argumentSignatures.size(), packet->timestamp);
        return {};
    }
    if (id != play::ChatMessagePacket::kId)
        return {};
    const auto packet = play::ChatMessagePacket::read(view);
    if (!packet)
        return {};
    const auto seen = chat_.applyLastSeen(packet->lastSeen);
    if (!seen)
        return kick(kValidationFailed);
    wire::chat::SignedBody signedBody{packet->message, packet->timestamp, packet->salt, *seen};
    auto decoded = chat_.unpack(packet->signature, std::move(signedBody), now);
    if (!decoded)
        return drop(packet->lastSeen.offset, std::move(decoded.error()));
    return {};
}

bool SecureChatGuard::clientbound(std::int32_t id, std::span<const std::uint8_t> body) {
    if (id != play::PlayerChatPacket::kId)
        return true;
    auto view = viewOf(body);
    const auto packet = play::PlayerChatPacket::read(view);
    return !packet || !packet->signature || chat_.noteDelivered(*packet->signature);
}

} // namespace basalt::proxy::runtime::detail
