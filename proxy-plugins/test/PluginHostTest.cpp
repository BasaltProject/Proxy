#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <basalt/proxy/plugins/PluginHost.hpp>

#include <doctest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>

using namespace basalt::proxy;
using namespace std::chrono_literals;

namespace {

void write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path} << text;
}

} // namespace

TEST_CASE("a runaway or broken plugin is contained and the others keep receiving events") {
    const std::filesystem::path root = std::filesystem::path{BASALT_TEST_SCRATCH_DIR} / "plugins";
    std::filesystem::remove_all(root);
    write(root / "greeter" / "main.lua",
          "proxy.on('join', function(e) proxy.log('hello ' .. e.player .. ' on ' .. e.backend .. ' of ' "
          "  .. #proxy.servers()) end)\n");
    write(root / "looper" / "main.lua", "proxy.on('join', function() while true do end end)\n");
    write(root / "syntax" / "main.lua", "this is not lua\n");
    write(root / "tools" / "main.lua", "proxy.command('hello', function(args) return 'hi ' .. #args end)\n"
                                       "local ok = pcall(proxy.command, 'stop', function() end)\n"
                                       "proxy.log('builtin accepted: ' .. tostring(ok))\n");

    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::string> lines;
    plugins::PluginHooks hooks;
    hooks.servers = [] { return std::vector<std::string>{"a", "b"}; };
    hooks.log = [&](std::string_view plugin, std::string_view line) {
        std::lock_guard lock{mutex};
        lines.push_back(std::string{plugin} + ": " + std::string{line});
        ready.notify_all();
    };
    plugins::PluginSettings settings;
    settings.directory = root.string();
    settings.limits.instructionsPerTurn = 20000;
    auto host = plugins::PluginHost::start(settings, hooks);
    REQUIRE(host);
    CHECK((*host)->loaded() == 3);
    CHECK((*host)->command("hello a b"));
    CHECK_FALSE((*host)->command("stop"));
    CHECK_FALSE((*host)->command("nosuch"));

    const auto hellos = [&] { return std::ranges::count(lines, "greeter: hello Steve on a of 2"); };
    for (int round = 1; round <= 2; ++round) {
        (*host)->post({runtime::PlayerEventKind::kJoin, "Steve", "a"});
        std::unique_lock lock{mutex};
        REQUIRE(ready.wait_for(lock, 5s, [&] { return hellos() == round; }));
    }
    std::lock_guard lock{mutex};
    CHECK(std::ranges::count_if(
              lines, [](const std::string& line) { return line.starts_with("looper: handler failed"); }) == 1);
}

TEST_CASE("plugin data persists across restarts and stays private to each plugin") {
    const std::filesystem::path root = std::filesystem::path{BASALT_TEST_SCRATCH_DIR} / "data";
    std::filesystem::remove_all(root);
    write(root / "plugins" / "counter" / "main.lua", "proxy.on('join', function(e)\n"
                                                     "  local n = tonumber(proxy.data.get('visits') or '0') + 1\n"
                                                     "  proxy.data.set('visits', tostring(n))\n"
                                                     "  proxy.log('visits ' .. n)\n"
                                                     "end)\n");
    write(root / "plugins" / "snoop" / "main.lua",
          "proxy.on('join', function(e) proxy.log('snoop sees ' .. tostring(proxy.data.get('visits'))) end)\n");

    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::string> lines;
    plugins::PluginHooks hooks;
    hooks.log = [&](std::string_view plugin, std::string_view line) {
        std::lock_guard lock{mutex};
        lines.push_back(std::string{plugin} + ": " + std::string{line});
        ready.notify_all();
    };
    plugins::PluginSettings settings;
    settings.directory = (root / "plugins").string();
    settings.database = (root / "state" / "proxy.db").string();

    for (int run = 1; run <= 2; ++run) {
        auto host = plugins::PluginHost::start(settings, hooks);
        REQUIRE(host);
        (*host)->post({runtime::PlayerEventKind::kJoin, "Steve", "a"});
        std::unique_lock lock{mutex};
        REQUIRE(ready.wait_for(lock, 5s, [&] { return std::ranges::count(lines, "snoop: snoop sees nil") == run; }));
    }
    std::lock_guard lock{mutex};
    CHECK(std::ranges::count(lines, "counter: visits 1") == 1);
    CHECK(std::ranges::count(lines, "counter: visits 2") == 1);
}
