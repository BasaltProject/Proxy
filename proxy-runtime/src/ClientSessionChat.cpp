#include "BackendLogin.hpp"
#include "ClientSession.hpp"

#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/common/json/Json.hpp>
#include <basalt/wire/multiversion/ProtocolVersions.hpp>
#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>
#include <basalt/wire/protocol/play/ChatPackets.hpp>
#include <basalt/wire/protocol/play/CommandPackets.hpp>
#include <basalt/wire/protocol/play/PlayPackets.hpp>
#include <basalt/wire/text/ComponentJson.hpp>
#include <basalt/wire/text/MiniMessage.hpp>

#include <algorithm>
#include <map>

namespace basalt::proxy::runtime::detail {
namespace {

namespace play = wire::protocol::play;

constexpr std::size_t kMaxAddedNodes = 512;

[[nodiscard]] play::ArgumentParser parserOf(const PlayerCommandSpec::Argument& argument) {
    using Kind = PlayerCommandSpec::Kind;
    switch (argument.kind) {
    case Kind::kText:
        return play::ArgumentParser{
            play::argumentType::kBrigadierString,
            play::ArgumentParser::StringProperties{play::ArgumentParser::StringKind::GreedyPhrase}};
    case Kind::kInt: {
        play::ArgumentParser::Bounds<std::int32_t> bounds;
        if (argument.min)
            bounds.min = static_cast<std::int32_t>(std::clamp<std::int64_t>(*argument.min, INT32_MIN, INT32_MAX));
        if (argument.max)
            bounds.max = static_cast<std::int32_t>(std::clamp<std::int64_t>(*argument.max, INT32_MIN, INT32_MAX));
        return play::ArgumentParser{play::argumentType::kBrigadierInteger, bounds};
    }
    case Kind::kFloat:
        return play::ArgumentParser{play::argumentType::kBrigadierDouble, play::ArgumentParser::Bounds<double>{}};
    case Kind::kBool:
        return play::ArgumentParser{play::argumentType::kBrigadierBool};
    case Kind::kWord:
        break;
    }
    return play::ArgumentParser{play::argumentType::kBrigadierString,
                                play::ArgumentParser::StringProperties{play::ArgumentParser::StringKind::SingleWord}};
}

} // namespace

bool ClientSession::relayCommandTree(std::span<const std::uint8_t> original) {
    auto specs = playerCommandSpecs_ ? playerCommandSpecs_() : std::vector<PlayerCommandSpec>{};
    if (specs.empty())
        return queuePacket(original);
    wire::io::ByteBuffer view;
    view.writeBytes(frame_.readable());
    auto tree = play::CommandsPacket::read(view);
    if (!tree || view.readableBytes() != 0 || tree->rootIndex < 0 ||
        static_cast<std::size_t>(tree->rootIndex) >= tree->nodes.size())
        return queuePacket(original);
    const auto rootIndex = static_cast<std::size_t>(tree->rootIndex);
    // The commands the backend already has keep their own nodes; ours are added for the rest.
    std::vector<std::string> taken;
    for (const std::int32_t child : tree->nodes[rootIndex].children)
        if (child >= 0 && static_cast<std::size_t>(child) < tree->nodes.size())
            taken.push_back(tree->nodes[static_cast<std::size_t>(child)].name.value_or(std::string{}));
    const std::size_t original_count = tree->nodes.size();
    std::map<std::string, std::int32_t> literals;
    const auto child = [&](std::int32_t parent, const std::string& name) -> std::int32_t {
        const std::string key = std::to_string(parent) + "/" + name;
        if (const auto found = literals.find(key); found != literals.end())
            return found->second;
        const auto index = static_cast<std::int32_t>(tree->nodes.size());
        play::CommandNodeData node;
        node.kind = play::CommandNodeData::Kind::Literal;
        node.name = name;
        tree->nodes.push_back(std::move(node));
        tree->nodes[static_cast<std::size_t>(parent)].children.push_back(index);
        literals.emplace(key, index);
        return index;
    };
    for (const PlayerCommandSpec& spec : specs) {
        if (spec.literals.empty() ||
            tree->nodes.size() - original_count + spec.arguments.size() + spec.literals.size() > kMaxAddedNodes)
            continue;
        if (std::ranges::find(taken, spec.literals.front()) != taken.end())
            continue;
        std::int32_t at = tree->rootIndex;
        for (const std::string& literal : spec.literals)
            at = child(at, literal);
        // A node ends the command when everything after it is optional.
        std::size_t firstOptional = spec.arguments.size();
        while (firstOptional > 0 && spec.arguments[firstOptional - 1].optional)
            --firstOptional;
        tree->nodes[static_cast<std::size_t>(at)].executable = firstOptional == 0;
        for (std::size_t i = 0; i < spec.arguments.size(); ++i) {
            const auto& argument = spec.arguments[i];
            const auto index = static_cast<std::int32_t>(tree->nodes.size());
            play::CommandNodeData node;
            node.kind = play::CommandNodeData::Kind::Argument;
            node.name = argument.name;
            node.parser = parserOf(argument);
            node.executable = i + 1 >= firstOptional;
            tree->nodes.push_back(std::move(node));
            tree->nodes[static_cast<std::size_t>(at)].children.push_back(index);
            at = index;
        }
        if (spec.arguments.empty())
            tree->nodes[static_cast<std::size_t>(at)].executable = true;
    }
    wire::io::ByteBuffer out;
    wire::protocol::varInt::write(out, play::CommandsPacket::kId);
    tree->write(out);
    return queuePacket(out.readable());
}

void ClientSession::tell(std::string_view text) {
    if (!inPlay() || text.empty())
        return;
    frame_.clear();
    wire::protocol::varInt::write(frame_, play::SystemChatPacket::kId);
    const auto component = wire::text::miniMessage::parse(text.substr(0, 4096));
    if (legacyRaw_) {
        if (clientProtocol_ < wire::multiversion::protocolVersions::kV1_19)
            return;
        wire::protocol::writeString(frame_, wire::common::writeJson(wire::text::componentJson::serialize(component)));
        if (clientProtocol_ == wire::multiversion::protocolVersions::kV1_19)
            wire::protocol::varInt::write(frame_, 1);
        else
            wire::protocol::writeBoolean(frame_, false);
    } else {
        play::SystemChatPacket{component, false}.write(frame_);
    }
    static_cast<void>(queuePacket(frame_.readable()));
}

void ClientSession::kick(std::string_view reason) {
    if (!inPlay())
        return;
    closing_ = true;
    closeBackends();
    frame_.clear();
    wire::protocol::varInt::write(frame_, play::PlayDisconnectPacket::kId);
    const auto component =
        wire::text::miniMessage::parse(reason.empty() ? std::string_view{"Disconnected"} : reason.substr(0, 1024));
    if (legacyRaw_)
        wire::protocol::writeString(frame_, wire::common::writeJson(wire::text::componentJson::serialize(component)));
    else
        play::PlayDisconnectPacket{component}.write(frame_);
    static_cast<void>(queuePacket(frame_.readable()));
}

std::string ClientSession::playerUuid() const { return profile_ ? profile_->id.toString() : std::string{}; }

} // namespace basalt::proxy::runtime::detail
