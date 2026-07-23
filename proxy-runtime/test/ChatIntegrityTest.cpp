#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../src/ChatIntegrity.hpp"

#include "SyntheticChatKeys.hpp"

#include <basalt/wire/protocol/VarInt.hpp>

#include <doctest.h>

namespace play = basalt::wire::protocol::play;
using basalt::proxy::runtime::detail::ChatVerdict;
using basalt::proxy::runtime::detail::SecureChatGuard;
using basalt::proxy::runtime::test::SyntheticPlayer;
using basalt::wire::common::Uuid;
using Kind = ChatVerdict::Kind;

namespace {

constexpr Uuid kProfile{0x0123456789abcdefULL, 0x0fedcba987654321ULL};
constexpr std::int64_t kNow = 1700000002000;

template <typename Packet>
std::vector<std::uint8_t> bodyOf(const Packet& packet) {
    basalt::wire::io::ByteBuffer out;
    packet.write(out);
    return out.toVector();
}

play::ChatMessagePacket signedChat(SyntheticPlayer& player, std::int32_t index, std::string text, std::int64_t at) {
    play::ChatMessagePacket packet;
    packet.message = std::move(text);
    packet.timestamp = at;
    packet.salt = 11;
    packet.signature = player.sign(index, {packet.message, at, packet.salt, {}});
    return packet;
}

} // namespace

TEST_CASE("the guard follows a signed chain and refuses what a backend would refuse") {
    SyntheticPlayer player{kProfile};
    SecureChatGuard guard{kProfile, player.keys()};
    REQUIRE(guard.serverbound(play::ChatSessionUpdatePacket::kId, bodyOf(player.session()), kNow).kind ==
            Kind::kForward);
    CHECK(guard.hasSession());

    const auto first = signedChat(player, 0, "hello", kNow - 500);
    CHECK(guard.serverbound(play::ChatMessagePacket::kId, bodyOf(first), kNow).kind == Kind::kForward);

    // A signed command with one argument signature uses one link of the chain without being checked.
    play::SignedChatCommandPacket command;
    command.command = "msg Alex hi";
    command.timestamp = kNow - 400;
    command.argumentSignatures.push_back({"message", {}});
    CHECK(guard.serverbound(play::SignedChatCommandPacket::kId, bodyOf(command), kNow).kind == Kind::kForward);

    CHECK(guard.serverbound(play::ChatMessagePacket::kId, bodyOf(signedChat(player, 2, "third", kNow - 300)), kNow)
              .kind == Kind::kForward);

    auto forged = signedChat(player, 3, "fourth", kNow - 200);
    forged.message = "fourth, edited";
    const auto refused = guard.serverbound(play::ChatMessagePacket::kId, bodyOf(forged), kNow);
    CHECK(refused.kind == Kind::kDrop);
    CHECK(refused.notify);
    CHECK(refused.key == "chat.disabled.invalid_signature");
    const auto broken = guard.serverbound(play::ChatMessagePacket::kId, bodyOf(signedChat(player, 4, "x", kNow)), kNow);
    CHECK(broken.key == "chat.disabled.chain_broken");
}

