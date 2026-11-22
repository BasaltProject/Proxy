#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "WasmDemoGuest.hpp"

#include <basalt/proxy/plugins/JarDetector.hpp>
#include <basalt/proxy/plugins/PluginHost.hpp>

#include <doctest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>

using namespace basalt::proxy;
using namespace std::chrono_literals;

namespace {

void manifest(const std::filesystem::path& folder, std::string_view id) {
    std::filesystem::create_directories(folder);
    std::ofstream{folder / "plugin.toml"} << "id = \"" << id << "\"\nversion = \"1.0.0\"\napi = 1\nabi = 1\n"
                                          << "main = \"plugin.wasm\"\n";
}

void write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path} << text;
}

} // namespace

TEST_CASE("a WebAssembly plugin greets, decides a login and is disabled alone when a callback loops") {
    const std::filesystem::path root = std::filesystem::path{BASALT_TEST_SCRATCH_DIR} / "wasm-plugins";
    std::filesystem::remove_all(root);
    manifest(root / "wasmdemo", "wasmdemo");
    {
        std::ofstream guest{root / "wasmdemo" / "plugin.wasm", std::ios::binary};
        guest.write(reinterpret_cast<const char*>(kDemoGuest), sizeof kDemoGuest);
    }
    write(root / "wasmdemo" / "resources" / "config.toml", "[deny]\nname = \"Griefer\"\n");

    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::string> lines;
    std::optional<runtime::DecisionResult> verdict;
    const auto note = [&](std::string line) {
        std::lock_guard lock{mutex};
        lines.push_back(std::move(line));
        ready.notify_all();
    };
    plugins::PluginHooks hooks;
    hooks.servers = [] { return std::vector<std::string>{"lobby"}; };
    hooks.tell = [&](std::string_view player, std::string_view text) {
        note("tell " + std::string{player} + ": " + std::string{text});
    };
    hooks.log = [&](std::string_view plugin, std::string_view line) {
        note(std::string{plugin} + ": " + std::string{line});
    };
    hooks.resolveDecision = [&](std::string_view, std::uint64_t, runtime::DecisionResult result) {
        std::lock_guard lock{mutex};
        verdict = std::move(result);
        ready.notify_all();
    };
    plugins::PluginSettings settings;
    settings.directory = root.string();
    auto host = plugins::PluginHost::start(settings, hooks);
    REQUIRE(host);
    CHECK((*host)->loaded() == 1);
    const auto seen = [&](const std::string& line, std::ptrdiff_t times = 1) {
        std::unique_lock lock{mutex};
        return ready.wait_for(lock, 10s, [&] { return std::ranges::count(lines, line) >= times; });
    };

    (*host)->post({runtime::PlayerEventKind::kJoin, "Alice", "lobby", "uuid-a", ""});
    CHECK(seen("tell Alice: <green>Hello Alice, you are on lobby (wasm proxy plugin)"));
    CHECK((*host)->command("wasmhello 2"));
    CHECK(seen("wasmdemo: <aqua>hello from wasm", 2));

    runtime::DecisionRequest login;
    login.kind = runtime::DecisionKind::kLogin;
    login.player = "Griefer";
    login.deadlineMs = 5000;
    REQUIRE((*host)->decide(login));
    {
        std::unique_lock lock{mutex};
        REQUIRE(ready.wait_for(lock, 10s, [&] { return verdict.has_value(); }));
        CHECK(verdict->deny);
        CHECK(verdict->reason == "<red>You are not welcome here");
    }

    CHECK((*host)->command("wasmloop"));
    CHECK(seen("wasmdemo: disabled until the next reload after repeated failures"));
    (*host)->post({runtime::PlayerEventKind::kJoin, "Bob", "lobby", "uuid-b", ""});
    std::lock_guard lock{mutex};
    CHECK(std::ranges::count(lines, "tell Bob: <green>Hello Bob, you are on lobby (wasm proxy plugin)") == 0);
}
