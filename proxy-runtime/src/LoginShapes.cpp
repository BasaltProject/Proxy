#include "LoginShapes.hpp"

#include <basalt/wire/multiversion/ProtocolVersions.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>

#include <array>
#include <cstdlib>

namespace basalt::proxy::runtime::detail {
namespace {

namespace versions = wire::multiversion::protocolVersions;

std::optional<wire::common::Uuid> parseDashedUuid(std::string_view text) {
    if (text.size() != 36)
        return std::nullopt;
    std::array<std::uint8_t, 16> bytes{};
    std::size_t index = 0;
    int pending = -1;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return std::nullopt;
            continue;
        }
        int digit = -1;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        if (digit < 0)
            return std::nullopt;
        if (pending < 0) {
            pending = digit;
        } else {
            bytes[index++] = static_cast<std::uint8_t>(pending * 16 + digit);
            pending = -1;
        }
    }
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    for (std::size_t i = 0; i < 8; ++i)
        high = (high << 8) | bytes[i];
    for (std::size_t i = 8; i < 16; ++i)
        low = (low << 8) | bytes[i];
    return wire::common::Uuid{high, low};
}

} // namespace

void writeLoginStart(wire::io::ByteBuffer& out, std::int32_t protocol, std::string_view name,
                     const wire::common::Uuid& id) {
    wire::protocol::writeString(out, name);
    if (protocol < versions::kV1_19)
        return;
    if (protocol <= versions::kV1_19_1) {
        wire::protocol::writeBoolean(out, false);
        if (protocol == versions::kV1_19_1) {
            wire::protocol::writeBoolean(out, true);
            wire::protocol::writeUuid(out, id);
        }
        return;
    }
    if (protocol < versions::kV1_20_2) {
        wire::protocol::writeBoolean(out, true);
        wire::protocol::writeUuid(out, id);
        return;
    }
    wire::protocol::writeUuid(out, id);
}

std::optional<std::vector<std::uint8_t>> loginFinishedToNative(std::int32_t protocol,
                                                               std::span<const std::uint8_t> body) {
    wire::io::ByteBuffer in;
    in.writeBytes(body);
    wire::io::ByteBuffer out;
    if (protocol < versions::kV1_16) {
        const auto text = wire::protocol::readString(in, 36);
        const auto id = parseDashedUuid(text);
        if (in.failed() || !id)
            return std::nullopt;
        wire::protocol::writeUuid(out, *id);
        out.writeBytes(in.readable());
        wire::protocol::varInt::write(out, 0);
    } else {
        const auto id = wire::protocol::readUuid(in);
        if (in.failed())
            return std::nullopt;
        wire::protocol::writeUuid(out, id);
        out.writeBytes(in.readable());
        if (protocol < versions::kV1_19)
            wire::protocol::varInt::write(out, 0);
    }
    for (int i = 0; i < 16; ++i)
        out.writeByte(0);
    const auto bytes = out.readable();
    return std::vector<std::uint8_t>{bytes.begin(), bytes.end()};
}

std::optional<std::string> readClientLoginName(std::int32_t protocol, wire::io::ByteBuffer& in) {
    auto name = wire::protocol::readString(in, wire::protocol::login::LoginStartPacket::kMaxNameLength);
    if (in.failed())
        return std::nullopt;
    const auto skipKey = [&] {
        if (wire::protocol::readBoolean(in)) {
            (void)in.readLong();
            const auto keyBytes = wire::protocol::varInt::read(in);
            if (keyBytes < 0 || keyBytes > 4096)
                return false;
            (void)in.readSpan(static_cast<std::size_t>(keyBytes));
            const auto signatureBytes = wire::protocol::varInt::read(in);
            if (signatureBytes < 0 || signatureBytes > 4096)
                return false;
            (void)in.readSpan(static_cast<std::size_t>(signatureBytes));
        }
        return !in.failed();
    };
    const auto skipUuid = [&] {
        if (wire::protocol::readBoolean(in))
            (void)wire::protocol::readUuid(in);
    };
    if (protocol >= versions::kV1_20_2) {
        (void)wire::protocol::readUuid(in);
    } else if (protocol >= versions::kV1_19_3) {
        skipUuid();
    } else if (protocol >= versions::kV1_19) {
        if (!skipKey())
            return std::nullopt;
        if (protocol >= versions::kV1_19_1)
            skipUuid();
    }
    if (in.failed() || in.readableBytes() != 0)
        return std::nullopt;
    return name;
}

void writeEncryptionRequest(wire::io::ByteBuffer& out, std::int32_t protocol,
                            const wire::protocol::login::HelloPacket& hello) {
    wire::protocol::writeString(out, hello.serverId);
    wire::protocol::writeByteArray(out, hello.publicKey);
    wire::protocol::writeByteArray(out, hello.challenge);
    if (protocol >= versions::kV1_20_5)
        wire::protocol::writeBoolean(out, hello.shouldAuthenticate);
}

std::optional<wire::protocol::login::KeyPacket> readEncryptionResponse(std::int32_t protocol,
                                                                       wire::io::ByteBuffer& in) {
    wire::protocol::login::KeyPacket key;
    key.encryptedSecret = wire::protocol::readByteArray(in);
    if (protocol == versions::kV1_19 || protocol == versions::kV1_19_1) {
        if (!wire::protocol::readBoolean(in))
            return std::nullopt;
    }
    key.encryptedChallenge = wire::protocol::readByteArray(in);
    if (in.failed())
        return std::nullopt;
    return key;
}

void writeLoginSuccess(wire::io::ByteBuffer& out, std::int32_t protocol,
                       const wire::protocol::login::LoginFinishedPacket& finished) {
    if (protocol < versions::kV1_16) {
        wire::protocol::writeString(out, finished.gameProfile.id.toString());
        wire::protocol::writeString(out, finished.gameProfile.name);
        return;
    }
    if (protocol < versions::kV1_19) {
        wire::protocol::writeUuid(out, finished.gameProfile.id);
        wire::protocol::writeString(out, finished.gameProfile.name);
        return;
    }
    wire::io::ByteBuffer native;
    finished.write(native);
    const auto bytes = native.readable();
    out.writeBytes(bytes.first(bytes.size() - 16));
}

bool keepAliveIsVarInt(std::int32_t protocol) noexcept { return protocol <= versions::kV1_12_1; }

} // namespace basalt::proxy::runtime::detail
