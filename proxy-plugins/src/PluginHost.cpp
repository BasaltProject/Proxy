#include "PluginHostSupport.hpp"

namespace basalt::proxy::plugins {
PluginHost::PluginHost(PluginSettings settings, PluginHooks hooks)
    : settings_(std::move(settings)), hooks_(std::move(hooks)) {}

PluginHost::~PluginHost() {
    shutdown();
    dropPacketListeners();
}

void PluginHost::dropPacketListeners() {
    if (wasm_)
        wasm_->dropPacketListeners();
    if (settings_.packets)
        settings_.packets->drain();
}

void PluginHost::shutdown() {
    thread_.request_stop();
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
    if (wasm_)
        wasm_->disable();
}

std::expected<std::unique_ptr<PluginHost>, std::string> PluginHost::start(PluginSettings settings, PluginHooks hooks) {
    std::unique_ptr<PluginHost> host{new PluginHost{std::move(settings), std::move(hooks)}};
    std::error_code error;
    if (!host->settings_.database.empty()) {
        const std::filesystem::path file{host->settings_.database};
        if (file.has_parent_path())
            std::filesystem::create_directories(file.parent_path(), error);
        host->database_ = wire::sql::Database::open(host->settings_.database);
        if (!host->database_)
            return std::unexpected("could not open the plugin database " + host->settings_.database);
        host->database_->exec("PRAGMA busy_timeout = 2000");
        if (!host->database_->exec(wire::storage::schema::pluginDdl()))
            return std::unexpected("plugin database " + host->settings_.database + ": could not create its tables");
    }
    host->loadPackages();
    for (const AdapterFactory& factory : host->settings_.adapters) {
        if (!factory)
            continue;
        auto adapter = factory(host->hooks_);
        if (!adapter)
            continue;
        for (std::string& root : adapter->commands())
            host->foreignCommands_.insert(std::move(root));
        for (const runtime::DecisionKind kind :
             {runtime::DecisionKind::kLogin, runtime::DecisionKind::kPreConnect, runtime::DecisionKind::kKick,
              runtime::DecisionKind::kChat, runtime::DecisionKind::kCommand, runtime::DecisionKind::kPing})
            if (adapter->decides(kind))
                host->deciders_.fetch_or(decisionBit(kind));
        host->adapters_.push_back(std::move(adapter));
    }
    host->loading_ = false;
    host->thread_ = std::jthread{[raw = host.get()](std::stop_token stop) { raw->run(stop); }};
    return host;
}

void PluginHost::loadPackages() {
    auto discovered = wire::plugins::discoverPackages(settings_.directory, ".bppkg");
    if (!discovered) {
        if (hooks_.log)
            hooks_.log("plugins", "could not read the plugin directory: " + discovered.error().message);
        return;
    }
    const auto refuse = [this](std::string_view what, std::string_view why) {
        if (hooks_.log)
            hooks_.log(what, "did not load: " + std::string{why});
    };
    std::vector<std::pair<std::shared_ptr<wire::plugins::PluginPackage>, wire::plugins::PluginManifest>> candidates;
    for (auto& owned : *discovered) {
        const std::shared_ptr<wire::plugins::PluginPackage> package{std::move(owned)};
        const auto text = package->readText(wire::plugins::packageLayout::kManifest);
        if (!text) {
            refuse(package->origin(), "it has no plugin.toml");
            continue;
        }
        auto manifest = wire::plugins::PluginManifest::parse(*text);
        if (!manifest) {
            refuse(package->origin(), manifest.error().message);
            continue;
        }
        if (std::ranges::any_of(candidates, [&](const auto& other) { return other.second.id == manifest->id; })) {
            refuse(package->origin(), "'" + manifest->id + "' is already installed from another package");
            continue;
        }
        candidates.emplace_back(package, std::move(*manifest));
    }
    std::vector<wire::plugins::PluginManifest> manifests;
    for (const auto& [package, manifest] : candidates)
        manifests.push_back(manifest);
    for (const auto& decision : wire::plugins::pluginOrder::resolve(manifests)) {
        if (!decision.loads()) {
            refuse(decision.manifest.id, decision.refusal);
            continue;
        }
        const auto found = std::ranges::find_if(
            candidates, [&](const auto& candidate) { return candidate.second.id == decision.manifest.id; });
        load(found->first, found->second);
    }
}

void PluginHost::load(const std::shared_ptr<wire::plugins::PluginPackage>& package,
                      const wire::plugins::PluginManifest& manifest) {
    const std::string& id = manifest.id;
    {
        std::unique_lock lock{commandsMutex_};
        if (!wasm_)
            wasm_ = std::make_unique<WasmHost>(*this);
    }
    if (wasm_->load(package, manifest)) {
        installed_.emplace(id, std::make_pair(package, manifest));
        ++loaded_;
    }
}

std::size_t PluginHost::reload() {
    std::lock_guard serial{reloadMutex_};
    shutdown();
    if (hooks_.commands)
        hooks_.commands->removeOwners("plugin:");
    dropPacketListeners();
    {
        std::unique_lock lock{commandsMutex_};
        wasm_.reset();
    }
    deciders_.store(0);
    {
        std::unique_lock lock{channelsMutex_};
        channels_.clear();
    }
    installed_.clear();
    roster_.clear();
    loaded_.store(0, std::memory_order_relaxed);
    loading_ = true;
    loadPackages();
    loading_ = false;
    thread_ = std::jthread{[this](std::stop_token stop) { run(stop); }};
    return loaded();
}

bool PluginHost::enqueue(Task task) {
    {
        std::lock_guard lock{mutex_};
        if (queue_.size() >= settings_.queueSize) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        queue_.push_back(std::move(task));
    }
    wake_.notify_one();
    return true;
}

void PluginHost::post(runtime::PlayerEvent event) { static_cast<void>(enqueue(std::move(event))); }

bool PluginHost::postCompletion(std::function<void(WasmHost&)> run) { return enqueue(CompletionCall{std::move(run)}); }

bool PluginHost::runOnPluginThread(std::function<void()> work, bool urgent) {
    {
        std::lock_guard lock{mutex_};
        if (queue_.size() >= settings_.queueSize * (urgent ? 2 : 1)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (urgent)
            queue_.push_front(std::move(work));
        else
            queue_.push_back(std::move(work));
    }
    wake_.notify_one();
    return true;
}

bool PluginHost::granted(std::string_view plugin, std::string_view permission) const {
    const auto found = settings_.allow.find(std::string{plugin});
    return found != settings_.allow.end() && std::ranges::find(found->second, permission) != found->second.end();
}

bool PluginHost::command(std::string_view line) {
    auto words = wordsOf(line, 32);
    {
        std::shared_lock lock{commandsMutex_};
        if (words.empty() || (!foreignCommands_.contains(words.front()) && words.front() != "plugins" &&
                              !(wasm_ && wasm_->hasRoot(words.front()))))
            return false;
    }
    static_cast<void>(enqueue(ConsoleCall{std::move(words)}));
    return true;
}

bool PluginHost::playerCommand(std::string_view player, std::string_view line) {
    auto words = wordsOf(line, 32);
    {
        std::shared_lock lock{commandsMutex_};
        if (words.empty() || !(wasm_ && wasm_->hasRoot(words.front())))
            return false;
    }
    static_cast<void>(enqueue(PlayerCommandCall{std::string{player}, std::move(words)}));
    return true;
}

} // namespace basalt::proxy::plugins
