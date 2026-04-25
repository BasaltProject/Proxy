#include "BackendLogin.hpp"
#include "ClientSession.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>

#include <algorithm>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::string_view kBungeeChannel = "bungeecord:main";
constexpr std::size_t kCommandLimit = 64;
constexpr std::size_t kControlPayloadLimit = 32768;

void ascii(wire::io::ByteBuffer& buffer, std::string_view text) {
    for (const auto byte : text)
        buffer.writeByte(static_cast<std::uint8_t>(byte));
}

bool validName(std::string_view name) {
    return !name.empty() && name.size() <= 64 &&
           std::ranges::all_of(name, [](unsigned char c) { return c >= 32 && c < 127; });
}
} // namespace

ClientSession::PluginMessageResult ClientSession::pluginMessage(bool fromBackend) {
    namespace protocol = wire::protocol;
    const auto channelBytes = protocol::varInt::read(frame_);
    if (frame_.failed() || channelBytes < 0 || channelBytes > 32767 * 3)
        return PluginMessageResult::kInvalid;
    const auto channel = frame_.readSpan(static_cast<std::size_t>(channelBytes));
    if (frame_.failed())
        return PluginMessageResult::kInvalid;
    if (!std::ranges::equal(channel, kBungeeChannel) && !std::ranges::equal(channel, std::string_view{"BungeeCord"}))
        return PluginMessageResult::kRelay;
    if (!fromBackend || state_ != State::kPlay || !backend() || !backends_[backend()->attempt().target].bungeeMessages)
        return PluginMessageResult::kConsumed;
    if (frame_.readableBytes() > kControlPayloadLimit)
        return PluginMessageResult::kInvalid;
    const auto commandBytes = static_cast<std::uint16_t>(frame_.readShort());
    if (frame_.failed() || commandBytes > kCommandLimit)
        return PluginMessageResult::kInvalid;
    const auto command = frame_.readSpan(commandBytes);
    if (frame_.failed())
        return PluginMessageResult::kInvalid;
    if (std::ranges::equal(command, std::string_view{"Connect"})) {
        const auto length = static_cast<std::uint16_t>(frame_.readShort());
        if (frame_.failed() || length == 0 || length > 64)
            return PluginMessageResult::kInvalid;
        const auto target = frame_.readSpan(length);
        if (frame_.failed() || frame_.readableBytes() != 0 ||
            !std::ranges::all_of(target, [](std::uint8_t c) { return c >= 32 && c < 127; }))
            return PluginMessageResult::kInvalid;
        for (std::size_t i = 0; i < backends_.size(); ++i)
            if (std::ranges::equal(target, backends_[i].name)) {
                (void)requestBackendChange(i);
                break;
            }
        return PluginMessageResult::kConsumed;
    }
    std::string_view name;
    for (const auto known : {std::string_view{"GetServer"}, std::string_view{"GetServers"}, std::string_view{"UUID"}})
        if (std::ranges::equal(command, known))
            name = known;
    if (name.empty())
        return PluginMessageResult::kConsumed;
    if (frame_.readableBytes() != 0 || !profile_)
        return PluginMessageResult::kInvalid;
    std::size_t valueBytes = 0;
    if (name == "UUID") {
        valueBytes = 32;
    } else if (name == "GetServer") {
        const auto& server = backends_[backend()->attempt().target].name;
        if (!validName(server))
            return PluginMessageResult::kInvalid;
        valueBytes = server.size();
    } else {
        for (const auto& server : backends_) {
            if (!validName(server.name) || valueBytes + server.name.size() + 2 > kControlPayloadLimit)
                return PluginMessageResult::kInvalid;
            valueBytes += server.name.size() + (valueBytes == 0 ? 0 : 2);
        }
    }
    frame_.clear();
    if (!frame_.tryReserve(1 + 1 + kBungeeChannel.size() + 4 + name.size() + valueBytes))
        return PluginMessageResult::kInvalid;
    protocol::varInt::write(frame_, 0x16);
    protocol::writeString(frame_, kBungeeChannel);
    frame_.writeShort(static_cast<std::int16_t>(name.size()));
    ascii(frame_, name);
    frame_.writeShort(static_cast<std::int16_t>(valueBytes));
    if (name == "UUID") {
        constexpr std::string_view kHex = "0123456789abcdef";
        for (const auto half : {profile_->id.high(), profile_->id.low()})
            for (int nibble = 15; nibble >= 0; --nibble)
                frame_.writeByte(static_cast<std::uint8_t>(kHex[(half >> (nibble * 4)) & 15]));
    } else if (name == "GetServer") {
        ascii(frame_, backends_[backend()->attempt().target].name);
    } else {
        bool first = true;
        for (const auto& server : backends_) {
            if (!first)
                ascii(frame_, ", ");
            ascii(frame_, server.name);
            first = false;
        }
    }
    if (!toBackend(*backend(), frame_.readable()))
        return PluginMessageResult::kInvalid;
    backend()->expectWriteUntil(Clock::now() + std::chrono::milliseconds{settings_.writeTimeoutMs});
    backpressure();
    return PluginMessageResult::kConsumed;
}

} // namespace basalt::proxy::runtime::detail
