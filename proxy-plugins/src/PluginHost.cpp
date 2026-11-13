#include "PluginHostSupport.hpp"

namespace basalt::proxy::plugins {
PluginHost::PluginHost(PluginSettings settings, PluginHooks hooks)
    : settings_(std::move(settings)), hooks_(std::move(hooks)) {}

PluginHost::~PluginHost() {
    shutdown();
    const std::lock_guard<std::recursive_timed_mutex> turn{luaMutex_};
    dropPacketListeners();
}

void PluginHost::dropPacketListeners() {
    for (auto& plugin : plugins_) {
        for (auto& handle : plugin->packetHandles)
            handle.remove();
        plugin->packetHandles.clear();
    }
    if (settings_.packets)
        settings_.packets->drain();
}

void PluginHost::shutdown() {
    thread_.request_stop();
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
    const std::lock_guard<std::recursive_timed_mutex> turn{luaMutex_};
    for (auto& plugin : plugins_)
        if (!plugin->broken)
            for (const wire::lua::LuaRef& handler : plugin->onDisable)
                call(*plugin, handler, {}, "plugin.disable");
    timers_.clear();
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
    {
        const std::lock_guard<std::recursive_timed_mutex> turn{host->luaMutex_};
        host->loadPackages();
    }
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
    const auto report = [this, &id](std::string_view line) {
        if (hooks_.log)
            hooks_.log(id, line);
    };
    auto source = package->read(manifest.main);
    if (!source || source->size() > kMaxSourceBytes) {
        report(manifest.main + " is missing or larger than 1 MiB; plugin skipped");
        return;
    }
    const auto granted = settings_.allow.find(id);
    wire::plugins::PluginManifest effective = manifest;
    std::string missing;
    std::string quoted;
    for (const wire::plugins::PluginPermission asked : manifest.capabilities.asks()) {
        const std::string_view label = wire::plugins::permissionName(asked);
        if (granted != settings_.allow.end() && std::ranges::find(granted->second, label) != granted->second.end())
            continue;
        missing += (missing.empty() ? "" : ", ") + std::string{label};
        quoted += (quoted.empty() ? "\"" : ", \"") + std::string{label} + "\"";
        if (asked == wire::plugins::PluginPermission::Packets)
            effective.capabilities.packets = false;
        else if (asked == wire::plugins::PluginPermission::Network)
            effective.capabilities.network.clear();
        else
            effective.capabilities.commands = false;
    }
    const auto& warn = hooks_.warn ? hooks_.warn : hooks_.log;
    if (!missing.empty() && warn)
        warn(id, "asks for " + missing + " and the operator has not granted it, so it loads without it. Add " + id +
                     " = [" + quoted + "] under [plugins.allow] in proxy.toml to allow it.");
    wire::common::toml::TomlTable config;
    {
        const wire::plugins::PluginFolder folder{*package, manifest, settings_.directory + "/" + id};
        folder.materialize([&report](std::string_view line) { report(line); });
        const auto index = folder.index();
        if (!index) {
            report("did not load: " + index.error().message);
            return;
        }
        for (const auto& [relative, path] : *index) {
            if (!relative.ends_with(".toml"))
                continue;
            const auto text = wire::io::fileIO::readText(path);
            if (!text)
                continue;
            auto parsed = wire::common::toml::parse(*text);
            if (!parsed) {
                report("did not load: " + path +
                       " is malformed: " + std::string{wire::common::toml::tomlErrorMessage(parsed.error())});
                return;
            }
            if (relative == "config.toml") {
                for (const std::string& key : parsed->keys())
                    if (const auto* value = parsed->find(key))
                        config.set(key, *value);
            } else {
                std::string name =
                    relative.substr(relative.rfind('/') == std::string::npos ? 0 : relative.rfind('/') + 1);
                name.resize(name.size() - 5);
                config.set(name, wire::common::toml::TomlValue{std::move(*parsed)});
            }
        }
    }
    // A plugin sees its own modules first, then those a dependency that loaded before it chose to export.
    std::vector<std::pair<std::shared_ptr<wire::plugins::PluginPackage>, wire::plugins::PluginManifest>> lenders;
    for (const wire::plugins::PluginDependency& dependency : manifest.dependencies)
        if (const auto lender = installed_.find(dependency.id); lender != installed_.end())
            lenders.push_back(lender->second);
    auto plugin = std::make_unique<Plugin>();
    plugin->id = id;
    plugin->name = manifest.name.empty() ? id : manifest.name;
    plugin->version = manifest.version;
    plugin->host = this;
    plugin->storage = manifest.capabilities.storage;
    plugin->playersData = manifest.capabilities.playersData;
    plugin->text = std::make_unique<wire::plugins::PluginText>(settings_.directory + "/" + id, "en_us");
    plugin->text->load();
    plugin->sandbox = wire::lua::LuaSandbox::create(
        id, settings_.limits, 0,
        [package, lenders](std::string_view module) -> std::optional<std::vector<std::uint8_t>> {
            const auto file = wire::plugins::packageLayout::moduleFile(module);
            if (!file)
                return std::nullopt;
            auto bytes = package->read(*file);
            for (std::size_t i = 0; !bytes && i < lenders.size(); ++i)
                if (lenders[i].second.exports.exportsModule(module))
                    bytes = lenders[i].first->read(*file);
            if (bytes && bytes->size() > kMaxSourceBytes)
                return std::nullopt;
            return bytes;
        },
        [report](std::string_view line) { report(line); });
    if (!plugin->sandbox) {
        report("could not create the Lua state; plugin skipped");
        return;
    }
    PluginApi::install(*this, *plugin, effective, config);
    PluginApi::installMessaging(*this, *plugin);
    PluginApi::installHud(*this, *plugin);
    auto chunk = plugin->sandbox->compile(wire::common::asText(*source), "@" + id + "/" + manifest.main);
    if (!chunk) {
        report("compile failed: " + chunk.error().message);
        return;
    }
    plugin->sandbox->refill();
    if (auto ran = plugin->sandbox->run(*chunk); !ran) {
        report("failed while loading: " + ran.error().message);
        if (hooks_.commands)
            hooks_.commands->removeOwners("plugin:" + id + "/");
        std::unique_lock lock{commandsMutex_};
        for (auto& [root, entries] : commands_)
            std::erase_if(entries, [&](const auto& entry) { return entry->plugin == plugin.get(); });
        std::erase_if(commands_, [](const auto& entry) { return entry.second.empty(); });
        std::erase_if(timers_, [&](const auto& timer) { return timer->plugin == plugin.get(); });
        return;
    }
    installed_.emplace(id, std::make_pair(package, manifest));
    plugins_.push_back(std::move(plugin));
    ++loaded_;
}

std::size_t PluginHost::reload() {
    std::lock_guard serial{reloadMutex_};
    shutdown();
    if (hooks_.commands)
        hooks_.commands->removeOwners("plugin:");
    {
        const std::lock_guard<std::recursive_timed_mutex> turn{luaMutex_};
        dropPacketListeners();
        {
            std::unique_lock lock{commandsMutex_};
            commands_.clear();
        }
        plugins_.clear();
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
    }
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

bool PluginHost::command(std::string_view line) {
    auto words = wordsOf(line, 32);
    {
        std::shared_lock lock{commandsMutex_};
        if (words.empty() || (!commands_.contains(words.front()) && !foreignCommands_.contains(words.front()) &&
                              words.front() != "plugins"))
            return false;
    }
    static_cast<void>(enqueue(ConsoleCall{std::move(words)}));
    return true;
}

bool PluginHost::playerCommand(std::string_view player, std::string_view line) {
    auto words = wordsOf(line, 32);
    {
        std::shared_lock lock{commandsMutex_};
        if (words.empty() || !commands_.contains(words.front()))
            return false;
    }
    static_cast<void>(enqueue(PlayerCommandCall{std::string{player}, std::move(words)}));
    return true;
}

} // namespace basalt::proxy::plugins
