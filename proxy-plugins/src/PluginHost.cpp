#include "PluginInternal.hpp"

#include <basalt/proxy/plugins/BungeeMessaging.hpp>
#include <basalt/wire/common/ByteView.hpp>
#include <basalt/wire/io/FileIO.hpp>
#include <basalt/wire/plugins/PacketView.hpp>
#include <basalt/wire/plugins/PluginFolder.hpp>
#include <basalt/wire/plugins/PluginOrder.hpp>
#include <basalt/wire/sql/Migrations.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>

namespace basalt::proxy::plugins {
namespace {

constexpr int kStrikeLimit = 5;

[[nodiscard]] std::vector<std::string> wordsOf(std::string_view line, std::size_t limit) {
    std::vector<std::string> words;
    while (!line.empty() && words.size() <= limit) {
        const auto begin = line.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos)
            break;
        line.remove_prefix(begin);
        const auto end = line.find_first_of(" \t\r\n");
        words.emplace_back(line.substr(0, end));
        if (end == std::string_view::npos)
            break;
        line.remove_prefix(end);
    }
    return words;
}

[[nodiscard]] bool sameName(std::string_view left, std::string_view right) {
    return left.size() == right.size() && std::ranges::equal(left, right, [](unsigned char a, unsigned char b) {
               return std::tolower(a) == std::tolower(b);
           });
}

} // namespace

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
        constexpr std::array<wire::sql::Migration, 2> kMigrations = {
            {{1, "CREATE TABLE plugin_data(plugin TEXT NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL, "
                 "PRIMARY KEY(plugin, key)) WITHOUT ROWID;"},
             {2, "CREATE TABLE player_data(plugin TEXT NOT NULL, player TEXT NOT NULL, key TEXT NOT NULL, "
                 "value TEXT NOT NULL, PRIMARY KEY(plugin, player, key)) WITHOUT ROWID;"}}};
        if (const auto migrated = wire::sql::migrate(*host->database_, kMigrations); !migrated)
            return std::unexpected("plugin database " + host->settings_.database + ": " + migrated.error());
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
    std::string missing;
    for (const wire::plugins::PluginPermission asked : manifest.capabilities.asks()) {
        const std::string_view label = wire::plugins::permissionName(asked);
        const bool allowed =
            granted != settings_.allow.end() && std::ranges::find(granted->second, label) != granted->second.end();
        if (!allowed)
            missing += (missing.empty() ? "" : ", ") + std::string{label};
    }
    if (!missing.empty()) {
        report("did not load: it asks for " + missing + " and the operator has not granted it; add " + id +
               " to [allow] in plugins.toml");
        return;
    }
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
    PluginApi::install(*this, *plugin, manifest, config);
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

std::vector<std::string> PluginHost::names() {
    std::lock_guard serial{reloadMutex_};
    std::vector<std::string> result;
    for (const auto& [id, installed] : installed_)
        result.push_back(installed.second.name.empty() ? id : installed.second.name);
    const std::lock_guard names{foreignMutex_};
    result.insert(result.end(), foreignNames_.begin(), foreignNames_.end());
    return result;
}

std::vector<std::string> PluginHost::playersOn(std::string_view server) const {
    std::vector<std::string> names;
    for (const auto& [name, player] : roster_)
        if (server.empty() || player.server == server)
            names.push_back(player.name);
    return names;
}

bool PluginHost::message(std::string_view player, std::string_view channel, std::span<const std::uint8_t> data,
                         bool fromBackend) {
    if (!isBungeeChannel(channel)) {
        std::shared_lock lock{channelsMutex_};
        const bool foreign =
            std::ranges::any_of(adapters_, [&](const auto& adapter) { return adapter->listens(channel); });
        if (!foreign && !channels_.contains(channel))
            return false;
    }
    static_cast<void>(enqueue(MessageCall{std::string{player}, std::string{channel},
                                          std::vector<std::uint8_t>(data.begin(), data.end()), fromBackend}));
    return true;
}

void PluginHost::deliverMessage(const MessageCall& call) {
    for (auto& adapter : adapters_)
        if (adapter->message(call.player, call.channel, call.data, call.fromBackend))
            return;
    if (isBungeeChannel(call.channel) && call.fromBackend) {
        BungeeContext context;
        context.player = [this](std::string_view name) -> std::optional<BungeePlayer> {
            const Player* found = findPlayer(name);
            if (found == nullptr)
                return std::nullopt;
            return BungeePlayer{found->name, found->uuid, found->server, found->address, found->port};
        };
        context.playersOn = [this](std::string_view server) { return playersOn(server); };
        context.serverExists = [this](std::string_view server) {
            return hooks_.servers && std::ranges::contains(hooks_.servers(), server);
        };
        context.serverAddress = [this](std::string_view server) {
            return hooks_.serverAddress ? hooks_.serverAddress(server) : std::nullopt;
        };
        context.connect = [this](std::string_view player, std::string_view server) {
            return hooks_.send && hooks_.send(player, server);
        };
        context.kick = [this](std::string_view player, std::string_view reason) {
            if (hooks_.kick)
                hooks_.kick(player, reason);
        };
        context.tell = [this](std::string_view player, std::string_view text) {
            if (hooks_.tell)
                hooks_.tell(player, text);
        };
        context.tellRaw = [this](std::string_view player, std::string_view json) {
            if (const auto component = componentOfJson(json); component && hooks_.tellComponent)
                hooks_.tellComponent(player, *component);
        };
        context.kickRaw = [this](std::string_view player, std::string_view json) {
            if (const auto component = componentOfJson(json); component && hooks_.kickComponent)
                hooks_.kickComponent(player, *component);
        };
        context.toBackend = [this](std::string_view player, std::string_view channel,
                                   std::span<const std::uint8_t> data) {
            if (hooks_.sendMessage)
                static_cast<void>(hooks_.sendMessage(player, channel, data, true));
        };
        if (!handleBungeeMessage(context, call.player, call.data) && hooks_.log)
            hooks_.log("bungeecord", "ignored a message from " + call.player + " that names no known subchannel");
    }
    const Player* known = findPlayer(call.player);
    const Player subject = known ? *known : Player{call.player, {}, {}};
    for (auto& plugin : plugins_) {
        if (plugin->broken)
            continue;
        const auto found = plugin->handlers.find(std::string{kMessageEvent});
        if (found == plugin->handlers.end())
            continue;
        for (const Plugin::Handler& handler : found->second) {
            const auto wanted = handler.filters.find("channel");
            if (wanted != handler.filters.end() && wanted->second != call.channel)
                continue;
            this->call(
                *plugin, handler.function,
                [&](wire::lua::LuaArgs& lua) {
                    lua.pushTable();
                    PluginApi::pushPlayer(lua, *plugin, subject);
                    lua.setField("player");
                    lua.pushString(call.channel);
                    lua.setField("channel");
                    lua.pushString(std::string{reinterpret_cast<const char*>(call.data.data()), call.data.size()});
                    lua.setField("data");
                    lua.pushString(call.fromBackend ? "backend" : "client");
                    lua.setField("source");
                    return 1;
                },
                std::string{kMessageEvent});
        }
    }
}

const PluginHost::Player* PluginHost::findPlayer(std::string_view name) const {
    const auto exact = roster_.find(name);
    if (exact != roster_.end())
        return &exact->second;
    for (const auto& [key, player] : roster_)
        if (sameName(key, name))
            return &player;
    return nullptr;
}

bool PluginHost::allowed(const Player& player, const std::optional<std::string>& permission) const {
    if (!permission)
        return true;
    return hooks_.hasPermission && hooks_.hasPermission(player.name, *permission);
}

void PluginHost::run(std::stop_token stop) {
    {
        std::vector<std::string> foreign;
        for (const auto& adapter : adapters_)
            for (const ForeignPlugin& plugin : adapter->plugins())
                foreign.push_back(plugin.name + " (" + std::string{adapter->label()} + ")");
        const std::lock_guard names{foreignMutex_};
        foreignNames_ = std::move(foreign);
    }
    // Enable runs here rather than while loading so a handler can wait, schedule and look at the players.
    for (auto& plugin : plugins_) {
        for (const wire::lua::LuaRef& handler : plugin->onEnable) {
            const std::lock_guard<std::recursive_timed_mutex> turn{luaMutex_};
            if (plugin->broken)
                break;
            call(*plugin, handler, {}, "plugin.enable");
        }
    }
    while (!stop.stop_requested()) {
        Task task;
        bool have = false;
        {
            std::unique_lock lock{mutex_};
            std::optional<std::chrono::steady_clock::time_point> next;
            for (const auto& timer : timers_)
                if (!next || timer->due < *next)
                    next = timer->due;
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
        const std::lock_guard<std::recursive_timed_mutex> turn{luaMutex_};
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
        else {
            const auto& typed = std::get<PlayerCommandCall>(task);
            const Player* known = findPlayer(typed.player);
            invoke(typed.words, known ? *known : Player{typed.player, {}, {}});
        }
        delivered_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PluginHost::runTimers(std::chrono::steady_clock::time_point now) {
    // A timer may register another, so due ones are taken out before any of them runs.
    std::vector<std::unique_ptr<Timer>> due;
    {
        std::lock_guard lock{mutex_};
        for (auto it = timers_.begin(); it != timers_.end();) {
            if ((*it)->cancelled && (*it)->cancelled->load()) {
                it = timers_.erase(it);
            } else if ((*it)->due <= now) {
                due.push_back(std::move(*it));
                it = timers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& timer : due) {
        if (timer->plugin->broken)
            continue;
        if (timer->resume) {
            timer->plugin->sandbox->refill();
            timer->resume();
            continue;
        }
        call(*timer->plugin, *timer->function, timer->arguments, "timer");
        if (timer->period.count() > 0 && !(timer->cancelled && timer->cancelled->load())) {
            timer->due = now + timer->period;
            std::lock_guard lock{mutex_};
            timers_.push_back(std::move(timer));
        }
    }
}

void PluginHost::call(Plugin& plugin, const wire::lua::LuaRef& function,
                      const std::function<std::int32_t(wire::lua::LuaArgs&)>& pushArguments, std::string_view why,
                      std::function<void(bool)> after) {
    plugin.sandbox->refill();
    wire::lua::LuaArgs lua{plugin.sandbox->state()};
    function.push(lua);
    const std::int32_t argumentCount = pushArguments ? pushArguments(lua) : 0;
    const std::string reason{why};
    Plugin* target = &plugin;
    static_cast<void>(wire::lua::LuaCall::start(
        *plugin.sandbox, argumentCount,
        [this, target, reason, after = std::move(after)](std::optional<wire::lua::LuaFailure> failure) {
            if (!failure)
                target->strikes = 0;
            else
                report(*target, *failure, reason);
            if (after)
                after(!failure);
        }));
}

bool PluginHost::deliverPacket(const void* owner, const wire::lua::LuaRef& function,
                               wire::plugins::PluginPacket& packet) {
    std::unique_lock<std::recursive_timed_mutex> turn{luaMutex_, std::defer_lock};
    if (!turn.try_lock_for(settings_.packetTimeout)) {
        const std::size_t waited = packetTimeouts_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hooks_.log && (waited == 1 || waited % 1000 == 0))
            hooks_.log("plugins", "packet listeners waited over " + std::to_string(settings_.packetTimeout.count()) +
                                      " ms for the plugin interpreter: " + std::to_string(waited) +
                                      " packet(s) so far went on untouched or were cancelled by fail = \"closed\" "
                                      "listeners");
        return false;
    }
    auto& plugin = *static_cast<Plugin*>(const_cast<void*>(owner));
    if (plugin.broken)
        return true;
    wire::plugins::PacketViewHost view;
    view.maxBodyBytes = settings_.packetMaxBytes;
    view.pushPlayer = [&plugin, this](wire::lua::LuaArgs& lua, const wire::plugins::PluginPacket& source) {
        if (source.playerName.empty()) {
            lua.pushNil();
            return;
        }
        const Player* known = findPlayer(source.playerName);
        PluginApi::pushPlayer(lua, plugin,
                              known ? *known : Player{source.playerName, source.playerUuid, source.backend});
    };
    plugin.sandbox->refill();
    std::shared_ptr<wire::plugins::PacketViewLease> lease;
    const auto answered = wire::lua::callSync(
        *plugin.sandbox, function,
        [&](wire::lua::LuaArgs& lua) {
            lease = wire::plugins::pushPacketView(lua, packet, view);
            return 1;
        },
        0);
    if (lease)
        lease->close();
    if (answered)
        plugin.strikes = 0;
    else
        report(plugin, answered.error(), "packet " + packet.name);
    return true;
}

void PluginHost::report(Plugin& plugin, const wire::lua::LuaFailure& failure, std::string_view why) {
    ++plugin.strikes;
    if (hooks_.log)
        hooks_.log(plugin.id, std::string{why} + " failed: " + failure.message);
    if (failure.outOfBudget() || failure.outOfMemory() || plugin.strikes >= kStrikeLimit) {
        plugin.broken = true;
        if (hooks_.log)
            hooks_.log(plugin.id, "disabled until the next reload after repeated failures");
    }
}

void PluginHost::callEvent(Plugin& plugin, const std::string& name, const Player* subject, const std::string& from) {
    const auto found = plugin.handlers.find(name);
    if (found == plugin.handlers.end())
        return;
    for (const Plugin::Handler& handler : found->second) {
        if (plugin.broken)
            return;
        if (!handler.observer && (name == kChatEvent || name == kKickEvent))
            continue;
        if (subject != nullptr) {
            const auto matches = [&](std::string_view key, const std::string& value) {
                const auto wanted = handler.filters.find(key);
                return wanted == handler.filters.end() || sameName(wanted->second, value);
            };
            if (!matches("player_name", subject->name) || !matches("player_uuid", subject->uuid) ||
                !matches("server", subject->server) ||
                (name != kChatEvent && name != kKickEvent && !matches("from_server", from)))
                continue;
        }
        call(
            plugin, handler.function,
            [&](wire::lua::LuaArgs& lua) {
                lua.pushTable();
                if (subject != nullptr) {
                    PluginApi::pushPlayer(lua, plugin, *subject);
                    lua.setField("player");
                    lua.pushString(subject->name);
                    lua.setField("player_name");
                    lua.pushString(subject->uuid);
                    lua.setField("player_uuid");
                    lua.pushString(subject->server);
                    lua.setField("server");
                    if (!from.empty()) {
                        lua.pushString(from);
                        lua.setField(name == kChatEvent ? "message" : name == kKickEvent ? "reason" : "from_server");
                    }
                }
                return 1;
            },
            name);
    }
}

void PluginHost::callPing(Plugin& plugin, const std::string& host, const std::string& protocol) {
    const auto found = plugin.handlers.find(std::string{kPingEvent});
    if (found == plugin.handlers.end())
        return;
    for (const Plugin::Handler& handler : found->second) {
        if (plugin.broken)
            return;
        if (!handler.observer)
            continue;
        call(
            plugin, handler.function,
            [&](wire::lua::LuaArgs& lua) {
                lua.pushTable();
                lua.pushString(host);
                lua.setField("host");
                lua.pushInteger(std::stoll(protocol));
                lua.setField("protocol");
                return 1;
            },
            std::string{kPingEvent});
    }
}

void PluginHost::callLogin(Plugin& plugin, const std::string& player, const std::string& uuid) {
    const auto found = plugin.handlers.find(std::string{kLoginEvent});
    if (found == plugin.handlers.end())
        return;
    for (const Plugin::Handler& handler : found->second) {
        if (plugin.broken)
            return;
        if (!handler.observer)
            continue;
        call(
            plugin, handler.function,
            [&](wire::lua::LuaArgs& lua) {
                lua.pushTable();
                lua.pushString(player);
                lua.setField("player_name");
                lua.pushString(uuid);
                lua.setField("player_uuid");
                return 1;
            },
            std::string{kLoginEvent});
    }
}

void PluginHost::callAntibot(Plugin& plugin, const runtime::PlayerEvent& event) {
    const auto found = plugin.handlers.find(std::string{kAntibotEvent});
    if (found == plugin.handlers.end())
        return;
    for (const Plugin::Handler& handler : found->second) {
        if (plugin.broken)
            return;
        call(
            plugin, handler.function,
            [&](wire::lua::LuaArgs& lua) {
                lua.pushTable();
                lua.pushString(event.backend);
                lua.setField("kind");
                lua.pushString(event.player);
                lua.setField("player_name");
                const auto first = event.uuid.find(':');
                const auto second = event.uuid.find(':', first + 1);
                lua.pushString(event.uuid.substr(0, first));
                lua.setField("action");
                lua.pushString(first == std::string::npos ? std::string{}
                                                          : event.uuid.substr(first + 1, second - first - 1));
                lua.setField("reason");
                lua.pushInteger(second == std::string::npos ? 0 : std::stoll(event.uuid.substr(second + 1)));
                lua.setField("skipped");
                lua.pushString(event.from);
                lua.setField("origin");
                return 1;
            },
            std::string{kAntibotEvent});
    }
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
    Player subject{event.player, event.uuid, event.backend, event.address, event.port};
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
    } else if (event.kind == Kind::kLogin) {
        for (auto& plugin : plugins_)
            if (!plugin->broken)
                callLogin(*plugin, event.player, event.uuid);
        return;
    } else if (event.kind == Kind::kAntibot) {
        for (auto& plugin : plugins_)
            if (!plugin->broken)
                callAntibot(*plugin, event);
        return;
    } else if (event.kind == Kind::kPing) {
        for (auto& plugin : plugins_)
            if (!plugin->broken)
                callPing(*plugin, event.backend, event.from);
        return;
    } else if (event.kind == Kind::kChat || event.kind == Kind::kKick) {
        name = std::string{event.kind == Kind::kChat ? kChatEvent : kKickEvent};
        if (const auto known = roster_.find(event.player); known != roster_.end())
            subject = known->second;
    } else {
        name = std::string{kQuitEvent};
    }
    for (auto& plugin : plugins_)
        if (!plugin->broken)
            callEvent(*plugin, name, &subject, event.from);
    if (event.kind == Kind::kLeave)
        roster_.erase(event.player);
}

void PluginHost::listPlugins() {
    std::string line;
    const auto add = [&line](std::string_view label, std::string_view name, std::string_view version) {
        line += (line.empty() ? "" : ", ") + std::string{"["} + std::string{label} + "] " + std::string{name} + " " +
                std::string{version};
    };
    for (const auto& plugin : plugins_)
        add("Lua", plugin->name, plugin->version);
    for (const auto& adapter : adapters_)
        for (const ForeignPlugin& plugin : adapter->plugins())
            add(adapter->label(), plugin.name, plugin.version);
    if (hooks_.log)
        hooks_.log("plugins", line.empty() ? "No plugins loaded." : "Plugins: " + line);
}

void PluginHost::invoke(const std::vector<std::string>& words, const std::optional<Player>& source) {
    const auto roots = commands_.find(words.front());
    if (roots == commands_.end()) {
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
        return;
    }
    const Entry* best = nullptr;
    for (const auto& entry : roots->second) {
        const auto& literals = entry->spec.literals;
        if (literals.size() > words.size() || entry->plugin->broken)
            continue;
        if (!std::equal(literals.begin(), literals.end(), words.begin()))
            continue;
        if (best == nullptr || literals.size() > best->spec.literals.size())
            best = entry.get();
    }
    if (best == nullptr)
        return;
    Plugin& plugin = *best->plugin;
    const auto answer = [&](std::string_view text) {
        if (source && hooks_.tell)
            hooks_.tell(source->name, text);
        else if (hooks_.log)
            hooks_.log(plugin.id, text);
    };
    if (source && !allowed(*source, best->spec.permission)) {
        answer("<red>You do not have permission to use this command.");
        return;
    }

    // Arguments are read word by word; a text argument takes whatever is left.
    struct Value {
        std::variant<std::monostate, std::string, std::int64_t, double, bool> value;
    };

    std::vector<Value> values;
    std::size_t at = best->spec.literals.size();
    for (const wire::plugins::CommandArgument& argument : best->spec.arguments) {
        if (at >= words.size()) {
            if (!argument.optional) {
                answer("<red>Usage: /" + best->spec.usage());
                return;
            }
            values.push_back({});
            continue;
        }
        const std::string& word = words[at];
        using Type = wire::plugins::CommandArgumentType;
        if (argument.type == Type::Text) {
            std::string joined;
            for (std::size_t i = at; i < words.size(); ++i)
                joined += (i == at ? "" : " ") + words[i];
            values.push_back({joined});
            at = words.size();
            continue;
        }
        ++at;
        if (argument.type == Type::Word) {
            values.push_back({word});
        } else if (argument.type == Type::Int) {
            std::int64_t number = 0;
            const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), number);
            if (error != std::errc{} || end != word.data() + word.size() || (argument.min && number < *argument.min) ||
                (argument.max && number > *argument.max)) {
                answer("<red>Usage: /" + best->spec.usage());
                return;
            }
            values.push_back({number});
        } else if (argument.type == Type::Float) {
            char* end = nullptr;
            const double number = std::strtod(word.c_str(), &end);
            if (end != word.c_str() + word.size()) {
                answer("<red>Usage: /" + best->spec.usage());
                return;
            }
            values.push_back({number});
        } else {
            if (word != "true" && word != "false") {
                answer("<red>Usage: /" + best->spec.usage());
                return;
            }
            values.push_back({word == "true"});
        }
    }
    if (at < words.size()) {
        answer("<red>Usage: /" + best->spec.usage());
        return;
    }
    call(
        plugin, best->function,
        [&](wire::lua::LuaArgs& lua) {
            if (source) {
                PluginApi::pushPlayer(lua, plugin, *source);
            } else {
                lua.pushTable();
                lua.pushBoolean(true);
                lua.setField("is_console");
                lua.pushString("CONSOLE");
                lua.setField("name");
                plugin.consoleMeta.push(lua);
                lua.setMetatable(-2);
            }
            for (const Value& value : values) {
                if (const auto* text = std::get_if<std::string>(&value.value))
                    lua.pushString(*text);
                else if (const auto* whole = std::get_if<std::int64_t>(&value.value))
                    lua.pushInteger(*whole);
                else if (const auto* real = std::get_if<double>(&value.value))
                    lua.pushNumber(*real);
                else if (const auto* flag = std::get_if<bool>(&value.value))
                    lua.pushBoolean(*flag);
                else
                    lua.pushNil();
            }
            return static_cast<std::int32_t>(values.size() + 1);
        },
        "command " + best->spec.name());
}

} // namespace basalt::proxy::plugins
