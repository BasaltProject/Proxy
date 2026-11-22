#include "PluginInternal.hpp"
#include "WasmHost.hpp"

#include <basalt/wire/common/Base64.hpp>

#include <algorithm>
#include <limits>

namespace basalt::proxy::plugins {
namespace {

using Kind = runtime::DecisionKind;

struct KindInfo {
    std::string_view event;
    unsigned bit;
};

KindInfo infoOf(Kind kind) {
    switch (kind) {
    case Kind::kLogin:
        return {kLoginEvent, 1U};
    case Kind::kPreConnect:
        return {kPreConnectEvent, 2U};
    case Kind::kKick:
        return {kKickEvent, 4U};
    case Kind::kChat:
        return {kChatEvent, 8U};
    case Kind::kCommand:
        return {kCommandEvent, 16U};
    case Kind::kPing:
        return {kPingEvent, 32U};
    }
    return {kLoginEvent, 1U};
}

} // namespace

unsigned decisionBit(std::string_view event) {
    for (const Kind kind : {Kind::kLogin, Kind::kPreConnect, Kind::kKick, Kind::kChat, Kind::kCommand, Kind::kPing})
        if (infoOf(kind).event == event)
            return infoOf(kind).bit;
    return 0;
}

unsigned decisionBit(runtime::DecisionKind kind) { return infoOf(kind).bit; }

struct PluginHost::DecisionRun {
    runtime::DecisionRequest request;
    std::chrono::steady_clock::time_point received;
    bool cancelled = false;
    std::string reason;
    std::string server;
    std::string text;
    std::int64_t maxPlayers = -1;
    std::int64_t online = -1;
    bool reasonJson = false;
    bool textJson = false;
    std::optional<std::string> versionName;
    std::optional<std::int32_t> versionProtocol;
    std::optional<std::vector<std::string>> sample;
    std::optional<std::vector<std::uint8_t>> favicon;
};

std::optional<std::vector<std::uint8_t>> faviconOf(std::string_view dataUri) {
    constexpr std::string_view kPrefix = "data:image/png;base64,";
    constexpr std::size_t kMaxIcon = 64 * 1024;
    if (dataUri.empty())
        return std::vector<std::uint8_t>{};
    if (dataUri.starts_with(kPrefix))
        dataUri.remove_prefix(kPrefix.size());
    auto bytes = wire::common::base64::decode(dataUri);
    if (!bytes || bytes->size() > kMaxIcon)
        return std::nullopt;
    return bytes;
}

bool PluginHost::decide(runtime::DecisionRequest request) {
    if ((deciders_.load() & infoOf(request.kind).bit) == 0)
        return false;
    return enqueue(DecisionCall{std::move(request), std::chrono::steady_clock::now()});
}

void PluginHost::deliverDecision(const DecisionCall& call) {
    DecisionRun run;
    run.request = call.request;
    run.received = call.received;
    const Kind kind = call.request.kind;
    if (kind == Kind::kPreConnect)
        run.server = call.request.backend;
    if (kind == Kind::kChat || kind == Kind::kCommand || kind == Kind::kPing)
        run.text = call.request.text;
    if (kind == Kind::kKick)
        run.reason = call.request.text;
    if (wasm_)
        decideWasm(run);
    finishDecision(run);
}

void PluginHost::decideWasm(DecisionRun& run) {
    const Kind kind = run.request.kind;
    WasmDecision wasm;
    wasm.kind = kind;
    wasm.player = run.request.player;
    wasm.uuid = run.request.uuid;
    wasm.address = run.request.address;
    wasm.port = run.request.port;
    wasm.host = run.request.host;
    wasm.from = run.request.from;
    wasm.protocol = kind == Kind::kPing ? std::string_view{run.request.text} : std::string_view{};
    wasm.cancelled = run.cancelled;
    wasm.signedContent = run.request.signedContent;
    wasm.text = kind == Kind::kChat || kind == Kind::kCommand ? run.text : std::string{};
    wasm.reasonJson = run.reasonJson;
    wasm.motdJson = run.textJson;
    wasm.reason = run.reason;
    wasm.server = run.server;
    wasm.motd = run.text == run.request.text ? std::string{} : run.text;
    wasm.version = run.versionName.value_or("");
    wasm.versionProtocol = run.versionProtocol.value_or(-1);
    wasm.maxPlayers = run.maxPlayers;
    wasm.online = run.online;
    wasm_->decide(wasm);
    if (kind != Kind::kPing)
        run.cancelled = wasm.cancelled;
    if (wasm.touched & (WasmDecision::kReason | WasmDecision::kReasonJson)) {
        run.reason = wasm.reason;
        run.reasonJson = wasm.reasonJson;
    }
    if (wasm.touched & WasmDecision::kText)
        run.text = wasm.text;
    if (wasm.touched & WasmDecision::kServer)
        run.server = wasm.server;
    if (wasm.touched & (WasmDecision::kMotd | WasmDecision::kMotdJson)) {
        run.text = wasm.motd;
        run.textJson = wasm.motdJson;
    }
    if ((wasm.touched & WasmDecision::kVersion) && !wasm.version.empty())
        run.versionName = wasm.version;
    if (wasm.touched & WasmDecision::kVersionProtocol)
        run.versionProtocol = static_cast<std::int32_t>(std::min<std::int64_t>(wasm.versionProtocol, INT32_MAX));
    if (wasm.touched & WasmDecision::kMaxPlayers)
        run.maxPlayers = wasm.maxPlayers;
    if (wasm.touched & WasmDecision::kOnline)
        run.online = wasm.online;
    if (wasm.touched & WasmDecision::kSample) {
        std::vector<std::string> lines;
        for (std::string_view rest = wasm.sample; !rest.empty() && lines.size() < kMaxSampleLines;) {
            const auto end = rest.find('\n');
            lines.emplace_back(rest.substr(0, std::min(end, kMaxSampleLine)));
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        }
        run.sample = std::move(lines);
    }
    if (wasm.touched & WasmDecision::kFavicon)
        if (auto icon = faviconOf(wasm.favicon))
            run.favicon = std::move(icon);
}

void PluginHost::finishDecision(DecisionRun& run) {
    const Kind kind = run.request.kind;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - run.received);
    if (elapsed.count() >= run.request.deadlineMs) {
        if (hooks_.log)
            hooks_.log("decisions", "a decision for " + run.request.player + " took " +
                                        std::to_string(elapsed.count()) + " ms; the proxy already went on");
        return;
    }
    for (auto& adapter : adapters_) {
        if (!adapter->decides(kind))
            continue;
        runtime::DecisionRequest asked = run.request;
        if (kind == Kind::kPreConnect)
            asked.backend = run.server;
        else if (kind == Kind::kChat || kind == Kind::kCommand)
            asked.text = run.text;
        else if (kind == Kind::kKick)
            asked.text = run.reason;
        const auto answer = adapter->decide(asked);
        if (!answer)
            continue;
        if (answer->deny) {
            run.cancelled = true;
            run.reason = answer->reason;
            run.reasonJson = answer->reasonJson;
        }
        if (!answer->redirect.empty())
            run.server = answer->redirect;
        if (!answer->text.empty()) {
            (kind == Kind::kKick ? run.reason : run.text) = answer->text;
            (kind == Kind::kKick ? run.reasonJson : run.textJson) = answer->textJson;
        }
        if (answer->versionName)
            run.versionName = answer->versionName;
        if (answer->versionProtocol)
            run.versionProtocol = answer->versionProtocol;
        if (answer->sample)
            run.sample = answer->sample;
        if (answer->favicon)
            run.favicon = answer->favicon;
        if (answer->maxPlayers)
            run.maxPlayers = *answer->maxPlayers;
        if (answer->online)
            run.online = *answer->online;
    }
    if (hooks_.resolveDecision) {
        runtime::DecisionResult result;
        result.deny = run.cancelled;
        result.reason = run.reason;
        result.reasonJson = run.reasonJson;
        if (kind == Kind::kPreConnect && !run.cancelled && run.server != run.request.backend)
            result.redirect = run.server;
        if (kind == Kind::kKick) {
            result.deny = false;
            result.redirect = run.server;
            result.reason.clear();
            if (run.reason != run.request.text) {
                result.text = run.reason;
                result.textJson = run.reasonJson;
            }
        } else if (kind == Kind::kCommand || kind == Kind::kChat) {
            if (run.text != run.request.text)
                result.text = run.text;
        } else if (kind == Kind::kPing) {
            result.text = run.text == run.request.text ? std::string{} : run.text;
            result.textJson = run.textJson;
            result.versionName = run.versionName;
            result.versionProtocol = run.versionProtocol;
            result.sample = run.sample;
            result.favicon = run.favicon;
            if (run.maxPlayers >= 0)
                result.maxPlayers = static_cast<std::int32_t>(run.maxPlayers);
            if (run.online >= 0)
                result.online = static_cast<std::int32_t>(run.online);
        }
        hooks_.resolveDecision(run.request.player, run.request.token, std::move(result));
    }
}

} // namespace basalt::proxy::plugins
