#include "PluginHostSupport.hpp"
#include "WasmInteraction.hpp"

namespace basalt::proxy::plugins {
namespace {
constexpr std::size_t kMaxSettingsKept = 1024;
} // namespace

void PluginHost::run(std::stop_token stop) {
    pluginThread_.store(std::this_thread::get_id());
    {
        std::vector<std::string> foreign;
        for (const auto& adapter : adapters_)
            for (const ForeignPlugin& plugin : adapter->plugins())
                foreign.push_back(plugin.name + " (" + std::string{adapter->label()} + ")");
        const std::lock_guard names{foreignMutex_};
        foreignNames_ = std::move(foreign);
    }
    if (wasm_)
        wasm_->enable();
    while (!stop.stop_requested()) {
        Task task;
        bool have = false;
        {
            std::unique_lock lock{mutex_};
            const auto next = wasm_ ? wasm_->nextDue() : std::nullopt;
            const auto ready = [this] { return !queue_.empty(); };
            if (next)
                wake_.wait_until(lock, stop, *next, ready);
            else
                wake_.wait(lock, stop, ready);
            if (stop.stop_requested())
                return;
            if (!queue_.empty()) {
                task = std::move(queue_.front());
                queue_.pop_front();
                have = true;
            }
        }
        const std::optional<WasmTurn> turn = wasm_ ? std::optional<WasmTurn>{std::in_place, *wasm_} : std::nullopt;
        runTimers(std::chrono::steady_clock::now());
        if (!have)
            continue;
        if (const auto* event = std::get_if<runtime::PlayerEvent>(&task))
            deliver(*event);
        else if (const auto* console = std::get_if<ConsoleCall>(&task))
            invoke(console->words, std::nullopt);
        else if (const auto* sent = std::get_if<MessageCall>(&task))
            deliverMessage(*sent);
        else if (const auto* asked = std::get_if<DecisionCall>(&task))
            deliverDecision(*asked);
        else if (const auto* done = std::get_if<CompletionCall>(&task)) {
            if (wasm_)
                done->run(*wasm_);
        } else if (const auto* work = std::get_if<Work>(&task))
            (*work)();
        else {
            const auto& typed = std::get<PlayerCommandCall>(task);
            const Player* known = findPlayer(typed.player);
            invoke(typed.words, known ? *known : Player{typed.player, {}, {}});
        }
        delivered_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PluginHost::runTimers(std::chrono::steady_clock::time_point now) {
    if (wasm_)
        wasm_->runTimers(now);
}

void PluginHost::deliver(const runtime::PlayerEvent& event) {
    using Kind = runtime::PlayerEventKind;
    for (auto& adapter : adapters_)
        adapter->notify(event);
    if (event.kind == Kind::kLatency) {
        if (const auto known = roster_.find(event.player); known != roster_.end())
            known->second.ping = event.value;
        return;
    }
    if (event.kind == Kind::kSettings) {
        if (!event.settings)
            return;
        if (clientSettings_.size() >= kMaxSettingsKept && !clientSettings_.contains(event.player))
            std::erase_if(clientSettings_, [this](const auto& entry) { return !roster_.contains(entry.first); });
        clientSettings_[event.player] = *event.settings;
        const auto known = roster_.find(event.player);
        if (known == roster_.end())
            return;
        known->second.settings = *event.settings;
        if (wasm_)
            notifySettings(*wasm_, known->second);
        return;
    }
    if (event.kind == Kind::kResourcePack && wasm_)
        notifyResourcePack(*wasm_, event.player, event.from, event.value);
    if (event.kind == Kind::kAntibot) {
        if (wasm_)
            wasm_->antibotEvent(event);
        return;
    }
    if (event.kind == Kind::kResourcePack || event.kind == Kind::kLogin || event.kind == Kind::kPing)
        return;
    Player subject{event.player, event.uuid, event.backend, event.address, event.port};
    if (const auto settings = clientSettings_.find(event.player); settings != clientSettings_.end())
        subject.settings = settings->second;
    if (const auto known = roster_.find(event.player); known != roster_.end() && event.address.empty()) {
        subject.address = known->second.address;
        subject.port = known->second.port;
        subject.ping = known->second.ping;
    }
    std::string name;
    if (event.kind == Kind::kJoin) {
        name = std::string{kJoinEvent};
        roster_[event.player] = subject;
    } else if (event.kind == Kind::kSwitch) {
        name = std::string{kServerChangeEvent};
        roster_[event.player] = subject;
    } else if (event.kind == Kind::kChat || event.kind == Kind::kKick) {
        name = std::string{event.kind == Kind::kChat ? kChatEvent : kKickEvent};
        if (const auto known = roster_.find(event.player); known != roster_.end())
            subject = known->second;
    } else {
        name = std::string{kQuitEvent};
    }
    if (wasm_)
        wasm_->observe(name, subject.name, subject.uuid, subject.server, subject.address, subject.port, event.from);
    if (event.kind == Kind::kJoin && wasm_ && subject.settings)
        notifySettings(*wasm_, roster_[event.player]);
    if (event.kind == Kind::kLeave) {
        if (wasm_)
            wasm_->forget(event.player);
        roster_.erase(event.player);
        clientSettings_.erase(event.player);
    }
}

void PluginHost::listPlugins() {
    std::string line;
    const auto add = [&line](std::string_view label, std::string_view name, std::string_view version) {
        line += (line.empty() ? "" : ", ") + std::string{"["} + std::string{label} + "] " + std::string{name} + " " +
                std::string{version};
    };
    if (wasm_)
        for (const auto& [name, version] : wasm_->listing())
            add("Wasm", name, version);
    for (const auto& adapter : adapters_)
        for (const ForeignPlugin& plugin : adapter->plugins())
            add(adapter->label(), plugin.name, plugin.version);
    if (hooks_.log)
        hooks_.log("plugins", line.empty() ? "No plugins loaded." : "Plugins: " + line);
}

void PluginHost::invoke(const std::vector<std::string>& words, const std::optional<Player>& source) {
    if (wasm_ && wasm_->invoke(words, source ? &*source : nullptr))
        return;
    if (!source && words.front() == "plugins")
        return listPlugins();
    if (!source && foreignCommands_.contains(words.front())) {
        std::string line;
        for (const std::string& word : words)
            line += (line.empty() ? "" : " ") + word;
        for (auto& adapter : adapters_)
            if (adapter->console(line))
                return;
    }
}

} // namespace basalt::proxy::plugins
