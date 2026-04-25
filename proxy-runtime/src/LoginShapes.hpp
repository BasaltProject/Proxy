#pragma once

#include <basalt/wire/common/Uuid.hpp>
#include <basalt/wire/io/ByteBuffer.hpp>
#include <basalt/wire/protocol/login/LoginPackets.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime::detail {

/// Writes the login start a backend of this protocol expects, which before 1.20.2 is not always name and uuid.
void writeLoginStart(wire::io::ByteBuffer& out, std::int32_t protocol, std::string_view name,
                     const wire::common::Uuid& id);

/// Reads a login success of any protocol before 1.20.2 and writes it back in the native numbering and shape.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> loginFinishedToNative(std::int32_t protocol,
                                                                             std::span<const std::uint8_t> body);

/// The player name from a login start sent by a client of this protocol; the rest of the packet must be well formed.
[[nodiscard]] std::optional<std::string> readClientLoginName(std::int32_t protocol, wire::io::ByteBuffer& in);

/// Writes the encryption request a client of this protocol can read.
void writeEncryptionRequest(wire::io::ByteBuffer& out, std::int32_t protocol,
                            const wire::protocol::login::HelloPacket& hello);

/// Reads an encryption response; refuses the signed variant 1.19 to 1.19.2 clients may send.
[[nodiscard]] std::optional<wire::protocol::login::KeyPacket> readEncryptionResponse(std::int32_t protocol,
                                                                                     wire::io::ByteBuffer& in);

/// Writes the login success a client older than 1.20.2 can read.
void writeLoginSuccess(wire::io::ByteBuffer& out, std::int32_t protocol,
                       const wire::protocol::login::LoginFinishedPacket& finished);

/// Up to 1.12.1 keepalive ids are varints; afterwards they are longs.
[[nodiscard]] bool keepAliveIsVarInt(std::int32_t protocol) noexcept;

} // namespace basalt::proxy::runtime::detail
