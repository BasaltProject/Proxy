#include "WasmEventFields.hpp"

#include "WasmPlugin.hpp"

#include <algorithm>
#include <charconv>

namespace basalt::proxy::plugins {
namespace {

using Type = EventFieldType;
using wire::wasm::Value;

constexpr EventField kPlayerName{"player_name", Type::kString, true, false};
constexpr EventField kPlayerUuid{"player_uuid", Type::kString, true, false};
constexpr EventField kAddress{"address", Type::kString, true, false};
constexpr EventField kPort{"port", Type::kInt, true, false};
constexpr EventField kHost{"host", Type::kString, true, false};
constexpr EventField kServerRead{"server", Type::kString, true, false};
constexpr EventField kServerWrite{"server", Type::kString, true, true};
constexpr EventField kFrom{"from_server", Type::kString, true, false};
constexpr EventField kMessageRead{"message", Type::kString, true, false};
constexpr EventField kMessage{"message", Type::kString, true, true};
constexpr EventField kCommand{"command", Type::kString, true, true};
constexpr EventField kSigned{"signed", Type::kBool, true, false};
constexpr EventField kCancelled{"cancelled", Type::kBool, true, true};
constexpr EventField kReason{"reason", Type::kString, true, true};
constexpr EventField kReasonRead{"reason", Type::kString, true, false};
constexpr EventField kReasonJson{"reason_json", Type::kString, false, true};
constexpr EventField kProtocol{"protocol", Type::kInt, true, false};
constexpr EventField kMotd{"motd", Type::kString, true, true};
constexpr EventField kMotdJson{"motd_json", Type::kString, false, true};
constexpr EventField kVersion{"version", Type::kString, true, true};
constexpr EventField kSample{"sample", Type::kList, true, true};
constexpr EventField kFavicon{"favicon", Type::kString, true, true};
constexpr EventField kMaxPlayers{"max_players", Type::kInt, true, true};
constexpr EventField kOnline{"online", Type::kInt, true, true};
constexpr EventField kVersionProtocol{"version_protocol", Type::kInt, true, true};

constexpr std::array kLoginFields{kPlayerName, kPlayerUuid, kAddress, kPort, kHost, kCancelled, kReason, kReasonJson};
constexpr std::array kPreConnectFields{kPlayerName,  kPlayerUuid, kAddress,   kPort,   kHost,
                                       kServerWrite, kFrom,       kCancelled, kReason, kReasonJson};
constexpr std::array kChatFields{kPlayerName, kPlayerUuid, kAddress,   kPort,   kHost,      kServerRead,
                                 kMessage,    kSigned,     kCancelled, kReason, kReasonJson};
constexpr std::array kCommandFields{kPlayerName, kPlayerUuid, kAddress,   kPort,   kHost,      kServerRead,
                                    kCommand,    kSigned,     kCancelled, kReason, kReasonJson};
constexpr std::array kKickFields{kPlayerName,  kPlayerUuid, kAddress, kPort,      kHost,
                                 kServerWrite, kFrom,       kReason,  kReasonJson};
constexpr std::array kPingFields{kHost,   kProtocol, kMotd,       kMotdJson, kVersion,
                                 kSample, kFavicon,  kMaxPlayers, kOnline,   kVersionProtocol};
constexpr std::array kSeenFields{kPlayerName, kPlayerUuid, kServerRead, kAddress, kPort};
constexpr std::array kChangedFields{kPlayerName, kPlayerUuid, kServerRead, kAddress, kPort, kFrom};
constexpr std::array kSaidFields{kPlayerName, kPlayerUuid, kServerRead, kAddress, kPort, kMessageRead};
constexpr std::array kKickedFields{kPlayerName, kPlayerUuid, kServerRead, kAddress, kPort, kReasonRead};

[[nodiscard]] Value text(std::string_view line) { return Value{std::string{line}}; }

[[nodiscard]] Value sampleList(std::string_view joined) {
    Value::List lines;
    for (std::string_view rest = joined; !rest.empty();) {
        const auto end = rest.find('\n');
        lines.emplace_back(std::string{rest.substr(0, end)});
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    }
    return Value{std::move(lines)};
}

[[nodiscard]] std::optional<Value> readPing(const WasmDecision& decision, std::string_view name) {
    if (name == "protocol") {
        std::int64_t number = 0;
        const auto end = decision.protocol.data() + decision.protocol.size();
        if (std::from_chars(decision.protocol.data(), end, number).ec != std::errc{})
            return std::nullopt;
        return Value{number};
    }
    if (name == "motd")
        return text(decision.motd);
    if (name == "version")
        return text(decision.version);
    if (name == "sample")
        return sampleList(decision.sample);
    if (name == "favicon")
        return text(decision.favicon);
    if (name == "max_players")
        return Value{decision.maxPlayers};
    if (name == "online")
        return Value{decision.online};
    if (name == "version_protocol")
        return Value{decision.versionProtocol};
    return std::nullopt;
}

} // namespace

std::span<const EventField> eventFields(const WasmCall& call) {
    if (call.command || call.value != nullptr)
        return {};
    switch (call.event) {
    case WasmEvent::kJoin:
    case WasmEvent::kQuit:
        return kSeenFields;
    case WasmEvent::kServerChange:
        return kChangedFields;
    case WasmEvent::kLogin:
        return kLoginFields;
    case WasmEvent::kPreConnect:
        return kPreConnectFields;
    case WasmEvent::kPing:
        return kPingFields;
    case WasmEvent::kChat:
        return call.observed ? std::span<const EventField>{kSaidFields} : std::span<const EventField>{kChatFields};
    case WasmEvent::kCommand:
        return kCommandFields;
    case WasmEvent::kKick:
        return call.observed ? std::span<const EventField>{kKickedFields} : std::span<const EventField>{kKickFields};
    case WasmEvent::kEnable:
    case WasmEvent::kDisable:
        break;
    }
    return {};
}

std::string_view eventFieldTypeName(EventFieldType type) {
    switch (type) {
    case Type::kString:
        return "string";
    case Type::kInt:
        return "int";
    case Type::kBool:
        return "bool";
    case Type::kList:
        return "list";
    }
    return "string";
}

std::optional<Value> readEventField(const WasmCall& call, std::string_view name) {
    const auto fields = eventFields(call);
    const auto known = std::ranges::find(fields, name, &EventField::name);
    if (known == fields.end() || !known->readable)
        return std::nullopt;
    const WasmDecision* decision = call.decision;
    if (name == "player_name")
        return text(call.name);
    if (name == "player_uuid")
        return text(call.uuid);
    if (name == "address")
        return text(call.address);
    if (name == "port")
        return Value{call.port};
    if (decision == nullptr) {
        if (name == "server")
            return text(call.server);
        if (name == "from_server")
            return text(call.from);
        return text(call.text);
    }
    if (name == "host")
        return text(decision->host);
    if (name == "server")
        return text(decision->kind == runtime::DecisionKind::kPreConnect ||
                            decision->kind == runtime::DecisionKind::kKick
                        ? std::string_view{decision->server}
                        : decision->from);
    if (name == "from_server")
        return text(decision->from);
    if (name == "message" || name == "command")
        return text(decision->text);
    if (name == "signed")
        return Value{decision->signedContent};
    if (name == "cancelled")
        return Value{decision->cancelled};
    if (name == "reason")
        return text(decision->reason);
    return readPing(*decision, name);
}

namespace {

[[nodiscard]] std::int32_t writeText(WasmPlugin& plugin, WasmDecision& decision, std::string_view name,
                                     const std::string& line) {
    if (line.size() > kWasmMaxText)
        return wire::wasm::kErrInvalid;
    if (name == "reason") {
        const std::string resolved =
            line.starts_with('@') ? plugin.text->translation(line.substr(1)).value_or(line) : line;
        decision.reason = resolved.substr(0, kWasmMaxText);
        decision.reasonJson = false;
        decision.touched |= WasmDecision::kReason;
    } else if (name == "reason_json") {
        decision.reason = line;
        decision.reasonJson = true;
        decision.touched |= WasmDecision::kReasonJson;
    } else if (name == "message" || name == "command") {
        if (name == "command" && line.empty())
            return wire::wasm::kErrInvalid;
        decision.text = line;
        decision.touched |= WasmDecision::kText;
    } else if (name == "server") {
        const auto& servers = plugin.host->hooks().servers;
        const bool exists = servers && std::ranges::contains(servers(), line);
        const bool clears = line.empty() && decision.kind == runtime::DecisionKind::kKick;
        if (!exists && !clears)
            return wire::wasm::kErrInvalid;
        decision.server = line;
        decision.touched |= WasmDecision::kServer;
    } else if (name == "motd") {
        decision.motd = line;
        decision.motdJson = false;
        decision.touched |= WasmDecision::kMotd;
    } else if (name == "motd_json") {
        decision.motd = line;
        decision.motdJson = true;
        decision.touched |= WasmDecision::kMotdJson;
    } else if (name == "version") {
        if (line.size() > kMaxSampleLine)
            return wire::wasm::kErrInvalid;
        decision.version = line;
        decision.touched |= WasmDecision::kVersion;
    } else if (name == "favicon") {
        if (!faviconOf(line))
            return wire::wasm::kErrInvalid;
        decision.favicon = line;
        decision.touched |= WasmDecision::kFavicon;
    } else {
        return wire::wasm::kErrInvalid;
    }
    return wire::wasm::kOk;
}

[[nodiscard]] std::int32_t writeSample(WasmDecision& decision, const Value& value) {
    const auto* list = value.asList();
    if (list == nullptr || list->size() > kMaxSampleLines)
        return wire::wasm::kErrInvalid;
    std::string joined;
    for (const Value& line : *list) {
        const auto* item = line.asString();
        if (item == nullptr || item->size() > kMaxSampleLine || item->contains('\n'))
            return wire::wasm::kErrInvalid;
        joined += (joined.empty() ? "" : "\n") + *item;
    }
    decision.sample = std::move(joined);
    decision.touched |= WasmDecision::kSample;
    return wire::wasm::kOk;
}

[[nodiscard]] std::int32_t writeNumber(WasmDecision& decision, std::string_view name, std::int64_t number) {
    if (number < 0)
        return wire::wasm::kErrInvalid;
    if (name == "max_players") {
        decision.maxPlayers = number;
        decision.touched |= WasmDecision::kMaxPlayers;
    } else if (name == "online") {
        decision.online = number;
        decision.touched |= WasmDecision::kOnline;
    } else {
        decision.versionProtocol = number;
        decision.touched |= WasmDecision::kVersionProtocol;
    }
    return wire::wasm::kOk;
}

} // namespace

std::int32_t writeEventField(WasmPlugin& plugin, WasmCall& call, std::string_view name, const Value& value) {
    const auto fields = eventFields(call);
    const auto known = std::ranges::find(fields, name, &EventField::name);
    if (known == fields.end())
        return wire::wasm::kErrAbsent;
    if (call.decision == nullptr || !known->writable)
        return wire::wasm::kErrInvalid;
    WasmDecision& decision = *call.decision;
    switch (known->type) {
    case Type::kBool: {
        const bool* flag = value.asBool();
        if (flag == nullptr)
            return wire::wasm::kErrInvalid;
        decision.cancelled = *flag;
        return wire::wasm::kOk;
    }
    case Type::kInt: {
        const std::int64_t* number = value.asInt();
        return number == nullptr ? wire::wasm::kErrInvalid : writeNumber(decision, name, *number);
    }
    case Type::kList:
        return writeSample(decision, value);
    case Type::kString: {
        const std::string* line = value.asString();
        return line == nullptr ? wire::wasm::kErrInvalid : writeText(plugin, decision, name, *line);
    }
    }
    return wire::wasm::kErrInvalid;
}

} // namespace basalt::proxy::plugins
