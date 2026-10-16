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

void manifest(const std::filesystem::path& folder, std::string_view id, std::string_view extra = {}) {
    std::filesystem::create_directories(folder);
    std::ofstream{folder / "plugin.toml"} << "id = \"" << id << "\"\nversion = \"1.0.0\"\napi = 1\n" << extra;
}

void write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path} << text;
}

} // namespace

TEST_CASE("a runaway or broken plugin is contained and the others keep receiving events") {
    const std::filesystem::path root = std::filesystem::path{BASALT_TEST_SCRATCH_DIR} / "plugins";
    std::filesystem::remove_all(root);
    manifest(root / "greeter", "greeter");
    write(root / "greeter" / "src" / "main.lua",
          "proxy.on('join', function(e) proxy.log('hello ' .. e.player .. ' on ' .. e.backend .. ' of ' "
          "  .. #proxy.servers()) end)\n");
    manifest(root / "looper", "looper");
    write(root / "looper" / "src" / "main.lua", "proxy.on('join', function() while true do end end)\n");
    manifest(root / "syntax", "syntax");
    write(root / "syntax" / "src" / "main.lua", "this is not lua\n");
    manifest(root / "tools", "tools", "[capabilities]\ncommands = true\n");
    write(root / "tools" / "src" / "main.lua", "proxy.command('hello', function(args) return 'hi ' .. #args end)\n"
                                               "local ok = pcall(proxy.command, 'stop', function() end)\n"
                                               "proxy.log('builtin accepted: ' .. tostring(ok))\n");

    manifest(root / "lender", "lender", "[exports]\nmodules = [\"shared\"]\n");
    write(root / "lender" / "src" / "shared.lua", "return {value = 7}\n");
    write(root / "lender" / "src" / "secret.lua", "return {value = 99}\n");
    write(root / "lender" / "src" / "main.lua", "\n");
    manifest(root / "borrower", "borrower", "[depends]\nlender = \">=1\"\n");
    write(root / "borrower" / "src" / "main.lua", "proxy.log('shared=' .. require('shared').value)\n"
                                                  "proxy.log('secret=' .. tostring(pcall(require, 'secret')))\n");
    manifest(root / "wants", "wants", "[capabilities]\ncommands = true\n");
    write(root / "wants" / "src" / "main.lua", "proxy.log('must not run either')\n");
    manifest(root / "plain", "plain");
    write(root / "plain" / "src" / "main.lua", "local ok, err = pcall(proxy.data.get, 'k')\n"
                                               "proxy.log('storage: ' .. tostring(ok) .. ' ' .. tostring(err))\n"
                                               "local sent = pcall(proxy.send, 'Steve', 'a')\n"
                                               "proxy.log('send: ' .. tostring(sent))\n");
    manifest(root / "child", "child", "[depends]\nmissing = \">=1\"\n");
    write(root / "child" / "src" / "main.lua", "proxy.log('must not run')\n");

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
    settings.allow["tools"] = {"commands"};
    auto host = plugins::PluginHost::start(settings, hooks);
    REQUIRE(host);
    CHECK((*host)->loaded() == 6);
    {
        std::lock_guard lock{mutex};
        CHECK(std::ranges::count_if(
                  lines, [](const std::string& line) { return line.starts_with("child: did not load"); }) == 1);
        CHECK(std::ranges::count(lines, "child: must not run") == 0);
        CHECK(std::ranges::count(lines, "wants: must not run either") == 0);
        CHECK(std::ranges::count_if(lines, [](const std::string& line) {
                  return line.starts_with("wants: did not load: it asks for commands");
              }) == 1);
        CHECK(std::ranges::count_if(lines, [](const std::string& line) {
                  return line.starts_with("plain: storage: false") &&
                         line.find("'storage' capability") != std::string::npos;
              }) == 1);
        CHECK(std::ranges::count(lines, "plain: send: false") == 1);
        CHECK(std::ranges::count(lines, "borrower: shared=7") == 1);
        CHECK(std::ranges::count(lines, "borrower: secret=false") == 1);
    }
    CHECK((*host)->command("hello a b"));
    CHECK_FALSE((*host)->command("stop"));
    CHECK_FALSE((*host)->command("nosuch"));

    const auto hellos = [&] { return std::ranges::count(lines, "greeter: hello Steve on a of 2"); };
    for (int round = 1; round <= 2; ++round) {
        (*host)->post({runtime::PlayerEventKind::kJoin, "Steve", "a"});
        std::unique_lock lock{mutex};
        REQUIRE(ready.wait_for(lock, 5s, [&] { return hellos() == round; }));
    }
    std::unique_lock lock{mutex};
    CHECK(std::ranges::count_if(
              lines, [](const std::string& line) { return line.starts_with("looper: handler failed"); }) == 1);
    lock.unlock();

    write(root / "tools" / "src" / "main.lua", "proxy.command('hello', function() return 'v2' end)\n");
    std::filesystem::remove_all(root / "greeter");
    CHECK((*host)->reload() == 5);
    CHECK((*host)->command("hello"));
    lock.lock();
    REQUIRE(ready.wait_for(lock, 5s, [&] { return std::ranges::count(lines, "tools: v2") == 1; }));
    lock.unlock();
    (*host)->post({runtime::PlayerEventKind::kJoin, "Alex", "a"});
    lock.lock();
    CHECK_FALSE(
        ready.wait_for(lock, 300ms, [&] { return std::ranges::count(lines, "greeter: hello Alex on a of 2") > 0; }));
}

TEST_CASE("plugin data persists across restarts and stays private to each plugin") {
    const std::filesystem::path root = std::filesystem::path{BASALT_TEST_SCRATCH_DIR} / "data";
    std::filesystem::remove_all(root);
    manifest(root / "plugins" / "counter", "counter", "[capabilities]\nstorage = true\n");
    write(root / "plugins" / "counter" / "src" / "main.lua",
          "proxy.on('join', function(e)\n"
          "  local n = tonumber(proxy.data.get('visits') or '0') + 1\n"
          "  proxy.data.set('visits', tostring(n))\n"
          "  proxy.log('visits ' .. n)\n"
          "end)\n");
    manifest(root / "plugins" / "snoop", "snoop", "[capabilities]\nstorage = true\n");
    write(root / "plugins" / "snoop" / "src" / "main.lua",
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
