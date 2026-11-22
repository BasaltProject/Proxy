#include "WasmPlugin.hpp"
#include "WasmValues.hpp"

#include <algorithm>

namespace basalt::proxy::plugins {

std::optional<WasmEvent> wasmEventOf(std::string_view name) {
    if (name == "plugin.enable")
        return WasmEvent::kEnable;
    if (name == "plugin.disable")
        return WasmEvent::kDisable;
    if (name == kJoinEvent)
        return WasmEvent::kJoin;
    if (name == kServerChangeEvent)
        return WasmEvent::kServerChange;
    if (name == kQuitEvent)
        return WasmEvent::kQuit;
    if (name == kLoginEvent)
        return WasmEvent::kLogin;
    if (name == kPreConnectEvent)
        return WasmEvent::kPreConnect;
    if (name == kPingEvent)
        return WasmEvent::kPing;
    if (name == kChatEvent)
        return WasmEvent::kChat;
    if (name == kCommandEvent)
        return WasmEvent::kCommand;
    if (name == kKickEvent)
        return WasmEvent::kKick;
    return std::nullopt;
}

WasmHost::WasmHost(PluginHost& host) : host_(host) {
    wire::wasm::EngineLimits limits;
    limits.profile = wire::wasm::Profile::Proxy;
    if (auto engine = wire::wasm::Engine::create(limits))
        engine_ = std::move(*engine);
    else if (host_.hooks_.log)
        host_.hooks_.log("plugins", "the WebAssembly engine did not start: " + engine.error().message);
}

WasmHost::~WasmHost() = default;

std::int32_t WasmHost::handleOf(std::string_view player) {
    const PluginHost::Player* known = host_.findPlayer(player);
    if (known == nullptr)
        return 0;
    if (const auto found = handles_.find(known->name); found != handles_.end())
        return found->second;
    const std::int32_t handle = nextHandle_++;
    handles_.emplace(known->name, handle);
    names_.emplace(handle, known->name);
    return handle;
}

const PluginHost::Player* WasmHost::playerOf(std::int32_t handle) {
    const auto found = names_.find(handle);
    return found == names_.end() ? nullptr : host_.findPlayer(found->second);
}

void WasmHost::forget(std::string_view player) {
    const auto found = handles_.find(player);
    if (found == handles_.end())
        return;
    names_.erase(found->second);
    handles_.erase(found);
}

WasmCall* WasmHost::live(std::int32_t ctx) noexcept {
    return current_ != nullptr && static_cast<std::uint32_t>(ctx) == current_->id ? current_ : nullptr;
}

namespace {
std::string briefOf(std::string_view message) {
    if (const auto cause = message.find("Caused by:"); cause != std::string_view::npos)
        message.remove_prefix(cause + 10);
    const auto begin = message.find_first_not_of(" \n");
    message = begin == std::string_view::npos ? std::string_view{} : message.substr(begin);
    return std::string{message.substr(0, message.find('\n'))};
}
} // namespace

void WasmHost::fail(WasmPlugin& plugin, const wire::wasm::Failure& failure, std::string_view why) {
    ++plugin.strikes;
    if (host_.hooks_.log)
        host_.hooks_.log(plugin.id, std::string{why} + " failed: " + briefOf(failure.message));
    if (failure.code == wire::wasm::ErrorCode::Interrupted || plugin.strikes >= kWasmStrikeLimit) {
        plugin.broken = true;
        if (host_.hooks_.log)
            host_.hooks_.log(plugin.id, "disabled until the next reload after repeated failures");
    }
}

bool WasmHost::dispatch(WasmPlugin& plugin, std::int32_t token, WasmCall* call, std::string_view why,
                        std::chrono::milliseconds budget) {
    if (plugin.broken)
        return false;
    const WasmTurn turn{*this};
    std::int32_t ctx = 0;
    if (call != nullptr) {
        call->id = nextCall_++;
        ctx = static_cast<std::int32_t>(call->id);
    }
    current_ = call;
    running_ = true;
    watchdog_.arm(*plugin.instance, budget);
    const auto result = plugin.instance->callback(token, ctx);
    watchdog_.disarm();
    current_ = nullptr;
    running_ = false;
    if (result) {
        plugin.strikes = 0;
        return true;
    }
    fail(plugin, result.error(), why);
    return false;
}

void WasmHost::notify(WasmPlugin& plugin, WasmEvent event, WasmCall* call, bool watchers) {
    call->event = event;
    call->observed = event != WasmEvent::kEnable && event != WasmEvent::kDisable;
    const auto slot = static_cast<std::size_t>(event);
    auto& list = watchers ? plugin.watchers[slot] : plugin.handlers[slot];
    // A handler may register more handlers while it runs, which reallocates the list: index, never iterate.
    for (std::size_t at = 0, count = list.size(); at < count && at < list.size(); ++at)
        if (!dispatch(plugin, list[at].token, call, "event", kWasmCallBudget))
            return;
}

void WasmHost::enable() {
    for (auto& plugin : plugins_) {
        if (plugin->enabled)
            continue;
        plugin->enabled = true;
        WasmCall call;
        notify(*plugin, WasmEvent::kEnable, &call, false);
    }
}

void WasmHost::disable() {
    for (auto& plugin : plugins_) {
        if (!plugin->enabled)
            continue;
        plugin->enabled = false;
        WasmCall call;
        notify(*plugin, WasmEvent::kDisable, &call, false);
    }
}

void WasmHost::observe(std::string_view event, std::string_view name, std::string_view uuid, std::string_view server,
                       std::string_view address, std::uint16_t port, std::string_view from) {
    const auto kind = wasmEventOf(event);
    if (!kind || *kind == WasmEvent::kEnable || *kind == WasmEvent::kDisable)
        return;
    WasmCall call;
    call.name = name;
    call.uuid = uuid;
    call.server = server;
    call.address = address;
    call.port = port;
    const bool message = *kind == WasmEvent::kChat || *kind == WasmEvent::kKick;
    call.from = message ? std::string_view{} : from;
    call.text = message ? from : std::string_view{};
    for (auto& plugin : plugins_) {
        if (!message)
            notify(*plugin, *kind, &call, false);
        notify(*plugin, *kind, &call, true);
    }
}

void WasmHost::decide(WasmDecision& decision) {
    using Kind = runtime::DecisionKind;
    WasmEvent event = WasmEvent::kPing;
    switch (decision.kind) {
    case Kind::kLogin:
        event = WasmEvent::kLogin;
        break;
    case Kind::kPreConnect:
        event = WasmEvent::kPreConnect;
        break;
    case Kind::kChat:
        event = WasmEvent::kChat;
        break;
    case Kind::kCommand:
        event = WasmEvent::kCommand;
        break;
    case Kind::kKick:
        event = WasmEvent::kKick;
        break;
    case Kind::kPing:
        break;
    }
    WasmCall call;
    call.event = event;
    call.name = decision.player;
    call.uuid = decision.uuid;
    call.address = decision.address;
    call.port = decision.port;
    call.from = decision.from;
    call.decision = &decision;
    const auto slot = static_cast<std::size_t>(event);
    for (std::int32_t priority = 0; priority < 5; ++priority)
        for (auto& plugin : plugins_) {
            const auto& list = plugin->handlers[slot];
            for (std::size_t at = 0; at < list.size(); ++at)
                if (list[at].priority == priority)
                    dispatch(*plugin, list[at].token, &call, "decision", kWasmCallBudget);
        }
}

bool WasmHost::hasRoot(std::string_view root) const { return commands_.contains(root); }

bool WasmHost::invoke(const std::vector<std::string>& words, const PluginHost::Player* source) {
    const auto roots = commands_.find(words.front());
    if (roots == commands_.end())
        return false;
    const WasmCommand* best = nullptr;
    for (const WasmCommand& entry : roots->second) {
        const auto& literals = entry.spec.literals;
        if (literals.size() > words.size() || entry.plugin->broken)
            continue;
        if (!std::equal(literals.begin(), literals.end(), words.begin()))
            continue;
        if (best == nullptr || literals.size() > best->spec.literals.size())
            best = &entry;
    }
    if (best == nullptr)
        return true;
    const auto answer = [&](std::string_view text) {
        if (source != nullptr && host_.hooks_.tell)
            host_.hooks_.tell(source->name, text);
        else if (host_.hooks_.log)
            host_.hooks_.log(best->plugin->id, text);
    };
    if (source != nullptr && !host_.allowed(*source, best->spec.permission)) {
        answer("<red>You do not have permission to use this command.");
        return true;
    }
    const auto values = wire::plugins::parseCommandWords(best->spec, words);
    if (!values) {
        answer("<red>Usage: /" + best->spec.usage());
        return true;
    }
    WasmCall call;
    call.command = true;
    call.console = source == nullptr;
    call.name = source != nullptr ? std::string_view{source->name} : std::string_view{"CONSOLE"};
    call.spec = &best->spec;
    call.values = &*values;
    dispatch(*best->plugin, best->token, &call, "command " + best->spec.name(), kWasmCallBudget);
    return true;
}

std::optional<std::chrono::steady_clock::time_point> WasmHost::nextDue() const {
    std::optional<std::chrono::steady_clock::time_point> next;
    for (const WasmTimer& timer : timers_)
        if (!next || timer.due < *next)
            next = timer.due;
    return next;
}

void WasmHost::runTimers(std::chrono::steady_clock::time_point now) {
    dueScratch_.clear();
    for (const WasmTimer& timer : timers_)
        if (timer.due <= now)
            dueScratch_.push_back(timer.id);
    for (const std::int32_t id : dueScratch_) {
        const auto found = std::ranges::find(timers_, id, &WasmTimer::id);
        if (found == timers_.end())
            continue;
        WasmPlugin& plugin = *found->plugin;
        const std::int32_t token = found->token;
        if (const std::int32_t request = found->request; request != 0) {
            timers_.erase(found);
            static_cast<void>(completeRequest(plugin, request, wire::wasm::Value{}));
            continue;
        }
        if (found->period.count() > 0)
            found->due = now + found->period;
        else
            timers_.erase(found);
        WasmCall call;
        dispatch(plugin, token, &call, "task", kWasmCallBudget);
    }
    std::erase_if(timers_, [](const WasmTimer& timer) { return timer.plugin->broken; });
}

std::vector<std::pair<std::string, std::string>> WasmHost::listing() const {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& plugin : plugins_)
        result.emplace_back(plugin->name, plugin->version);
    return result;
}

} // namespace basalt::proxy::plugins
