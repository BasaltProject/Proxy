#include "WasmPlugin.hpp"

#include <basalt/wire/io/FileIO.hpp>
#include <basalt/wire/plugins/PluginFolder.hpp>

#include <filesystem>

namespace basalt::proxy::plugins {
namespace {
constexpr std::size_t kMaxModuleBytes = 16U * 1024U * 1024U;
constexpr std::string_view kCacheDirectory = ".basalt/wasm-cache";

void logWasi(void* user, std::int32_t fd, std::string_view text) {
    auto* plugin = static_cast<WasmPlugin*>(user);
    plugin->host->logLine(*plugin, fd == 2 ? "error: " : "", text.substr(0, kWasmMaxText));
}

} // namespace

void WasmHost::logLine(const WasmPlugin& plugin, std::string_view prefix, std::string_view line) const {
    if (host_.hooks_.log)
        host_.hooks_.log(plugin.id, std::string{prefix} + std::string{line});
}

bool WasmHost::load(const std::shared_ptr<wire::plugins::PluginPackage>& package,
                    const wire::plugins::PluginManifest& manifest) {
    const std::string& id = manifest.id;
    const auto report = [this, &id](std::string_view line) {
        if (host_.hooks_.log)
            host_.hooks_.log(id, line);
    };
    if (!engine_) {
        report("did not load: the WebAssembly engine is not running");
        return false;
    }
    if (manifest.abi != 1) {
        report("did not load: it was built for ABI " + std::to_string(manifest.abi) + " and this proxy speaks ABI 1");
        return false;
    }
    const auto bytes = package->read(manifest.main);
    if (!bytes || bytes->size() > kMaxModuleBytes) {
        report(manifest.main + " is missing or larger than 16 MiB; plugin skipped");
        return false;
    }
    auto plugin = std::make_unique<WasmPlugin>();
    plugin->id = id;
    plugin->name = manifest.name.empty() ? id : manifest.name;
    plugin->version = manifest.version;
    plugin->host = this;
    plugin->capabilities = manifest.capabilities;
    plugin->authors = manifest.authors;
    plugin->dependencies = manifest.dependencies;
    const std::string folderPath = host_.settings_.directory + "/" + id;
    {
        const wire::plugins::PluginFolder folder{*package, manifest, folderPath};
        folder.materialize([&report](std::string_view line) { report(line); });
        const auto index = folder.index();
        if (!index) {
            report("did not load: " + index.error().message);
            return false;
        }
        std::optional<wire::common::toml::TomlTable> limits;
        for (const auto& [relative, path] : *index) {
            const bool isConfig = relative == "config.toml" || relative == "resources/config.toml";
            const bool isLimits = relative == "limits.toml" || relative == "resources/limits.toml";
            if (!isConfig && !isLimits)
                continue;
            const auto text = wire::io::fileIO::readText(path);
            if (!text)
                continue;
            auto parsed = wire::common::toml::parse(*text);
            if (!parsed) {
                report("did not load: " + path + " is malformed");
                return false;
            }
            if (isLimits)
                limits = std::move(*parsed);
            else
                plugin->config = std::move(*parsed);
        }
        if (limits)
            plugin->config.set("limits", wire::common::toml::TomlValue::ofTable(std::move(*limits)));
    }
    plugin->text = std::make_unique<wire::plugins::PluginText>(folderPath, "en_us");
    plugin->text->load();

    auto module = wire::wasm::Module::compileCached(*engine_, *bytes, kCacheDirectory);
    if (!module) {
        report("did not load: " + module.error().message);
        return false;
    }
    plugin->module = std::move(*module);
    plugin->imports = WasmImports::table(*plugin);
    wire::wasm::InstanceOptions options;
    options.imports = plugin->imports;
    options.wasi.user = plugin.get();
    options.wasi.log = logWasi;
    auto instance = wire::wasm::Instance::create(*plugin->module, options);
    if (!instance) {
        report("did not load: " + instance.error().message);
        return false;
    }
    plugin->instance = std::move(*instance);
    plugins_.push_back(std::move(plugin));
    WasmPlugin& added = *plugins_.back();
    const WasmTurn turn{*this};
    watchdog_.arm(*added.instance, kWasmInitBudget);
    const auto started = added.instance->init();
    watchdog_.disarm();
    if (!started) {
        report("failed while loading: " + started.error().message);
        if (host_.hooks_.commands)
            host_.hooks_.commands->removeOwners("plugin:" + id + "/");
        for (auto& [root, entries] : commands_)
            std::erase_if(entries, [&](const WasmCommand& entry) { return entry.plugin == &added; });
        std::erase_if(commands_, [](const auto& entry) { return entry.second.empty(); });
        std::erase_if(timers_, [&](const WasmTimer& timer) { return timer.plugin == &added; });
        plugins_.pop_back();
        return false;
    }
    return true;
}

} // namespace basalt::proxy::plugins
