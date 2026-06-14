#include <basalt/proxy/plugins/BungeeMessaging.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

namespace basalt::proxy::plugins {
namespace {

constexpr std::array<std::string_view, 17> kSubchannels = {
    "Connect",    "ConnectOther", "IP",         "IPOther",    "PlayerCount",     "PlayerList",
    "GetServers", "GetServer",    "Message",    "Forward",    "ForwardToPlayer", "UUID",
    "UUIDOther",  "ServerIP",     "KickPlayer", "MessageRaw", "KickPlayerRaw"};
constexpr std::string_view kAll = "ALL";
constexpr std::string_view kOnline = "ONLINE";

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return !failed_; }

    [[nodiscard]] bool done() const noexcept { return at_ == bytes_.size(); }

    std::uint16_t shortValue() {
        if (bytes_.size() - at_ < 2) {
            failed_ = true;
            return 0;
        }
        const auto value = static_cast<std::uint16_t>((bytes_[at_] << 8) | bytes_[at_ + 1]);
        at_ += 2;
        return value;
    }

    std::string utf() {
        const auto length = shortValue();
        return std::string{take(length)};
    }

    std::span<const std::uint8_t> bytes(std::size_t count) {
        if (bytes_.size() - at_ < count) {
            failed_ = true;
            return {};
        }
        const auto view = bytes_.subspan(at_, count);
        at_ += count;
        return view;
    }

private:
    std::string_view take(std::size_t count) {
        const auto view = bytes(count);
        return {reinterpret_cast<const char*>(view.data()), view.size()};
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t at_ = 0;
    bool failed_ = false;
};

class Writer {
public:
    Writer& utf(std::string_view text) {
        const auto length = std::min<std::size_t>(text.size(), 0xffff);
        shortValue(static_cast<std::uint16_t>(length));
        data_.insert(data_.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(length));
        return *this;
    }

    Writer& shortValue(std::uint16_t value) {
        data_.push_back(static_cast<std::uint8_t>(value >> 8));
        data_.push_back(static_cast<std::uint8_t>(value));
        return *this;
    }

    Writer& intValue(std::int32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            data_.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) >> shift));
        return *this;
    }

    Writer& raw(std::span<const std::uint8_t> bytes) {
        data_.insert(data_.end(), bytes.begin(), bytes.end());
        return *this;
    }

    [[nodiscard]] std::span<const std::uint8_t> view() const noexcept { return data_; }

private:
    std::vector<std::uint8_t> data_;
};

std::string joined(const std::vector<std::string>& names) {
    std::string text;
    for (const auto& name : names)
        text += (text.empty() ? "" : ", ") + name;
    return text;
}

std::string escaped(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '<' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

} // namespace

bool isBungeeChannel(std::string_view channel) noexcept {
    return channel == kBungeeChannel || channel == kLegacyBungeeChannel;
}

std::span<const std::string_view> bungeeSubchannels() noexcept { return kSubchannels; }

bool handleBungeeMessage(const BungeeContext& context, std::string_view source, std::span<const std::uint8_t> payload) {
    Reader in{payload};
    const std::string sub = in.utf();
    if (!in.ok())
        return false;
    const auto reply = [&](const Writer& out) {
        if (context.toBackend)
            context.toBackend(source, kBungeeChannel, out.view());
    };
    const auto sender = context.player ? context.player(source) : std::nullopt;
    if (!sender)
        return false;

    if (sub == "Connect") {
        const std::string server = in.utf();
        return in.ok() && context.connect && (context.connect(source, server), true);
    }
    if (sub == "ConnectOther") {
        const std::string name = in.utf();
        const std::string server = in.utf();
        return in.ok() && context.connect && (context.connect(name, server), true);
    }
    if (sub == "IP") {
        reply(Writer{}.utf("IP").utf(sender->address).intValue(sender->port));
        return true;
    }
    if (sub == "IPOther") {
        const std::string name = in.utf();
        const auto other = in.ok() ? context.player(name) : std::nullopt;
        if (!other)
            return false;
        reply(Writer{}.utf("IPOther").utf(other->name).utf(other->address).intValue(other->port));
        return true;
    }
    if (sub == "PlayerCount" || sub == "PlayerList") {
        const std::string server = in.utf();
        if (!in.ok() || !context.playersOn)
            return false;
        const bool all = server == kAll;
        if (!all && (!context.serverExists || !context.serverExists(server)))
            return true;
        const auto names = context.playersOn(all ? std::string_view{} : std::string_view{server});
        Writer out;
        out.utf(sub).utf(server);
        if (sub == "PlayerCount")
            out.intValue(static_cast<std::int32_t>(names.size()));
        else
            out.utf(joined(names));
        reply(out);
        return true;
    }
    if (sub == "Message") {
        const std::string target = in.utf();
        const std::string text = in.utf();
        if (!in.ok() || !context.tell)
            return false;
        if (target == kAll) {
            for (const auto& name : context.playersOn ? context.playersOn({}) : std::vector<std::string>{})
                context.tell(name, escaped(text));
        } else {
            context.tell(target, escaped(text));
        }
        return true;
    }
    if (sub == "Forward" || sub == "ForwardToPlayer") {
        const std::string target = in.utf();
        const std::string channel = in.utf();
        const auto length = in.shortValue();
        const auto data = in.bytes(length);
        if (!in.ok() || !context.toBackend)
            return false;
        Writer out;
        out.utf(channel).shortValue(length).raw(data);
        if (sub == "ForwardToPlayer") {
            context.toBackend(target, kBungeeChannel, out.view());
            return true;
        }
        std::vector<std::string> servers;
        if (target == kAll || target == kOnline) {
            for (const auto& name : context.playersOn ? context.playersOn({}) : std::vector<std::string>{})
                if (const auto other = context.player(name))
                    if (other->server != sender->server && std::ranges::find(servers, other->server) == servers.end())
                        servers.push_back(other->server);
        } else {
            servers.push_back(target);
        }
        for (const auto& server : servers)
            if (const auto names = context.playersOn(server); !names.empty())
                context.toBackend(names.front(), kBungeeChannel, out.view());
        return true;
    }
    if (sub == "MessageRaw") {
        const std::string target = in.utf();
        const std::string json = in.utf();
        if (!in.ok() || !context.tellRaw)
            return false;
        if (target == kAll) {
            for (const auto& name : context.playersOn ? context.playersOn({}) : std::vector<std::string>{})
                context.tellRaw(name, json);
        } else {
            context.tellRaw(target, json);
        }
        return true;
    }
    if (sub == "KickPlayerRaw") {
        const std::string target = in.utf();
        const std::string json = in.utf();
        return in.ok() && context.kickRaw && (context.kickRaw(target, json), true);
    }
    if (sub == "KickPlayer") {
        const std::string target = in.utf();
        const std::string reason = in.utf();
        return in.ok() && context.kick && (context.kick(target, escaped(reason)), true);
    }
    if (sub == "UUIDOther") {
        const std::string name = in.utf();
        const auto other = in.ok() ? context.player(name) : std::nullopt;
        if (!other)
            return false;
        reply(Writer{}.utf("UUIDOther").utf(other->name).utf(other->uuid));
        return true;
    }
    if (sub == "ServerIP") {
        const std::string server = in.utf();
        const auto found = in.ok() && context.serverAddress ? context.serverAddress(server) : std::nullopt;
        if (!found)
            return false;
        reply(Writer{}.utf("ServerIP").utf(server).utf(found->first).shortValue(found->second));
        return true;
    }
    return false;
}

} // namespace basalt::proxy::plugins
