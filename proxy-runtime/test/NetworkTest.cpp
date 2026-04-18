#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "support/BackendFixture.hpp"

#include <basalt/wire/io/EventLoop.hpp>
#include <basalt/wire/protocol/PacketFraming.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/handshake/HandshakePacket.hpp>

#include <doctest.h>

#include <array>

using namespace basalt;
using proxy::runtime::test::BackendFixture;

TEST_CASE("a controlled backend delivers a fragmented independent protocol 777 "
          "handshake") {
    auto backend = BackendFixture::listen();
    REQUIRE(backend);
    wire::io::EventLoop loop;
    REQUIRE(loop.watch(backend->handle(), {true, false}, 1));
    auto client = wire::io::TcpConnection::connect("127.0.0.1", backend->port());
    REQUIRE(client);
    auto ready = loop.wait(2000);
    REQUIRE(ready);
    REQUIRE(ready->size() == 1);
    auto peer = backend->accept();
    REQUIRE(peer);
    REQUIRE(peer->setNonBlocking());
    REQUIRE(loop.watch(peer->nativeHandle(), {true, false}, 2));
    loop.unwatch(backend->handle());

    // Synthetic bytes independent of Wire's writer: length, id, protocol 777,
    // host, port, status.
    const std::array<std::uint8_t, 17> handshake = {16,  0,   0x89, 0x06, 9,   'l',  'o',  'c', 'a',
                                                    'l', 'h', 'o',  's',  't', 0x63, 0xDD, 1};
    REQUIRE(client->write(std::span{handshake}.first(2)));
    ready = loop.wait(2000);
    REQUIRE(ready);
    REQUIRE(ready->size() == 1);
    std::array<std::uint8_t, 32> chunk{};
    auto received = peer->read(chunk);
    REQUIRE(received);
    REQUIRE(*received == 2);
    wire::io::ByteBuffer incoming;
    incoming.writeBytes(std::span{chunk}.first(*received));
    wire::io::ByteBuffer frame;
    const auto incomplete = wire::protocol::packetFraming::tryReadFrame(incoming, false, frame);
    REQUIRE(incomplete);
    CHECK_FALSE(*incomplete);

    REQUIRE(client->write(std::span{handshake}.subspan(2)));
    while (incoming.readableBytes() < handshake.size()) {
        ready = loop.wait(2000);
        REQUIRE(ready);
        REQUIRE_FALSE(ready->empty());
        received = peer->read(chunk);
        REQUIRE(received);
        REQUIRE(*received > 0);
        incoming.writeBytes(std::span{chunk}.first(*received));
    }
    const auto complete = wire::protocol::packetFraming::tryReadFrame(incoming, false, frame);
    REQUIRE(complete);
    REQUIRE(*complete);
    CHECK(wire::protocol::varInt::read(frame) == 0);
    const auto decoded = wire::protocol::handshake::HandshakePacket::read(frame);
    REQUIRE(decoded);
    CHECK(decoded->protocolVersion == 777);
    CHECK(decoded->hostName == "localhost");
    CHECK(decoded->port == 25565);
    CHECK(decoded->intent == wire::protocol::handshake::ClientIntent::Status);
    CHECK(frame.readableBytes() == 0);
}
