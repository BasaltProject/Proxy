#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <basalt/proxy/plugins/JarDetector.hpp>
#include <basalt/proxy/plugins/PluginHost.hpp>
#include <basalt/wire/plugins/PacketNaming.hpp>
#include <basalt/wire/plugins/PacketRegistry.hpp>
#if defined(BASALT_HAS_SHIM)
#include <basalt/proxy/plugins/JavaAdapter.hpp>
#endif

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
          "on('player.join', function(e) log.info('hello ' .. e.player.name .. ' on ' .. e.server .. ' of ' "
          "  .. #servers.list() .. ' online ' .. players.count()) end)\n"
          "on('player.quit', function(e) log.info('bye ' .. e.player_name) end)\n"
          "on('player.server_change', function(e) log.info('moved ' .. e.from_server .. ' to ' .. e.server) end)\n"
          "watch('player.chat', function(e) log.info('said ' .. e.player_name .. ': ' .. e.message) end)\n"
          "watch('player.kick', function(e) log.info('kicked ' .. e.player_name .. ': ' .. e.reason) end)\n"
          "watch('server.ping', function(e) log.info('pinged ' .. e.host .. ' ' .. e.protocol) end)\n"
          "watch('player.login', function(e) log.info('login ' .. e.player_name) end)\n");
    manifest(root / "looper", "looper");
    write(root / "looper" / "src" / "main.lua", "on('player.join', function() while true do end end)\n");
    manifest(root / "syntax", "syntax");
    write(root / "syntax" / "src" / "main.lua", "this is not lua\n");
    manifest(root / "tools", "tools");
    write(root / "tools" / "lang" / "en_us.toml", "welcome = \"Hello {who}\"\n");
    write(root / "tools" / "src" / "main.lua",
          "cmd('hello [times: int 1..5]', function(src, times) src:send('hi ' .. tostring(times)) end)\n"
          "cmd('hello world', function(src) src:send('hello world') end)\n"
          "cmd('greet', function(src) src:send('@welcome', {who = 'Ann'}) log.info(t('welcome', {who = 'Bob'})) end)\n"
          "cmd('secret', function(src) src:send('opened') end, {permission = 'tools.secret'})\n"
          "local ok = pcall(cmd, 'stop', function() end)\n"
          "log.info('builtin accepted: ' .. tostring(ok))\n"
          "on('plugin.enable', function() wait(1) log.info('waited') end)\n"
          "later(1, function() log.info('later') end)\n");
    manifest(root / "lender", "lender", "[exports]\nmodules = [\"shared\"]\n");
    write(root / "lender" / "src" / "shared.lua", "return {value = 7}\n");
    write(root / "lender" / "src" / "secret.lua", "return {value = 99}\n");
    write(root / "lender" / "src" / "main.lua", "\n");
    manifest(root / "borrower", "borrower", "[depends]\nlender = \">=1\"\n");
    write(root / "borrower" / "src" / "main.lua", "log.info('shared=' .. require('shared').value)\n"
                                                  "log.info('secret=' .. tostring(pcall(require, 'secret')))\n");
    manifest(root / "wants", "wants", "[capabilities]\ncommands = true\n");
    write(root / "wants" / "src" / "main.lua", "log.info('must not run either')\n");
    manifest(root / "plain", "plain");
    write(root / "plain" / "src" / "main.lua", "local ok, err = pcall(function() return store.k end)\n"
                                               "log.info('storage: ' .. tostring(ok) .. ' ' .. tostring(err))\n");
    manifest(root / "child", "child", "[depends]\nmissing = \">=1\"\n");
    write(root / "child" / "src" / "main.lua", "log.info('must not run')\n");

    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::string> lines;
    plugins::PluginHooks hooks;
    hooks.servers = [] { return std::vector<std::string>{"a", "b"}; };
    hooks.tell = [&](std::string_view player, std::string_view text) {
        std::lock_guard lock{mutex};
        lines.push_back("tell " + std::string{player} + ": " + std::string{text});
        ready.notify_all();
    };
    hooks.log = [&](std::string_view plugin, std::string_view line) {
        std::lock_guard lock{mutex};
        lines.push_back(std::string{plugin} + ": " + std::string{line});
        ready.notify_all();
    };
    hooks.hasPermission = [](std::string_view player, std::string_view) { return player == "Admin"; };
    plugins::PluginSettings settings;
    settings.directory = root.string();
    settings.limits.instructionsPerTurn = 20000;
    auto host = plugins::PluginHost::start(settings, hooks);
    REQUIRE(host);
    CHECK((*host)->loaded() == 6);
    const auto seen = [&](const std::string& line) {
        std::unique_lock lock{mutex};
        return ready.wait_for(lock, 5s, [&] { return std::ranges::count(lines, line) >= 1; });
    };
    CHECK(seen("tools: waited"));
    CHECK(seen("tools: later"));
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
                  return line.starts_with("plain: storage: false") && line.find("storage = true") != std::string::npos;
              }) == 1);
        CHECK(std::ranges::count(lines, "borrower: shared=7") == 1);
        CHECK(std::ranges::count(lines, "borrower: secret=false") == 1);
        CHECK(std::ranges::count(lines, "tools: builtin accepted: false") == 1);
    }
    CHECK((*host)->command("hello 2"));
    CHECK((*host)->command("hello world"));
    CHECK_FALSE((*host)->command("stop"));
    CHECK_FALSE((*host)->command("nosuch"));
    CHECK((*host)->playerCommand("Steve", "hello 3"));
    CHECK((*host)->playerCommand("Steve", "hello 9"));
    CHECK((*host)->playerCommand("Steve", "secret"));
    CHECK((*host)->playerCommand("Admin", "secret"));
    CHECK(seen("tools: hi 2"));
    CHECK(seen("tools: hello world"));
    CHECK(seen("tell Steve: hi 3"));
    CHECK((*host)->playerCommand("Steve", "greet"));
    CHECK(seen("tell Steve: Hello Ann"));
    CHECK(seen("tools: Hello Bob"));
    CHECK(seen("tell Steve: <red>Usage: /hello [times: int 1..5]"));
    CHECK(seen("tell Steve: <red>You do not have permission to use this command."));
    CHECK(seen("tell Admin: opened"));

    for (int round = 1; round <= 2; ++round) {
        (*host)->post({runtime::PlayerEventKind::kJoin, "Steve", "a", "uuid-1", {}});
        REQUIRE(seen("greeter: hello Steve on a of 2 online 1"));
    }
    (*host)->post({runtime::PlayerEventKind::kSwitch, "Steve", "b", "uuid-1", "a"});
    CHECK(seen("greeter: moved a to b"));
    (*host)->post({runtime::PlayerEventKind::kChat, "Steve", {}, {}, "hi all"});
    CHECK(seen("greeter: said Steve: hi all"));
    (*host)->post({runtime::PlayerEventKind::kKick, "Steve", {}, {}, "Flying"});
    CHECK(seen("greeter: kicked Steve: Flying"));
    (*host)->post({runtime::PlayerEventKind::kPing, {}, "mc.example", {}, "777"});
    CHECK(seen("greeter: pinged mc.example 777"));
    (*host)->post({runtime::PlayerEventKind::kLogin, "Steve", {}, "uuid-1", {}});
    CHECK(seen("greeter: login Steve"));
    (*host)->post({runtime::PlayerEventKind::kLeave, "Steve", "b", "uuid-1", {}});
    CHECK(seen("greeter: bye Steve"));
    {
        std::lock_guard lock{mutex};
        CHECK(std::ranges::count_if(
                  lines, [](const std::string& line) { return line.starts_with("looper: player.join failed"); }) == 1);
    }

    write(root / "tools" / "src" / "main.lua", "cmd('hello', function(src) src:send('v2') end)\n");
    std::filesystem::remove_all(root / "greeter");
    CHECK((*host)->reload() == 5);
    CHECK((*host)->command("hello"));
    CHECK(seen("tools: v2"));
    (*host)->post({runtime::PlayerEventKind::kJoin, "Alex", "a", "uuid-2", {}});
    std::unique_lock lock{mutex};
    CHECK_FALSE(ready.wait_for(
        lock, 300ms, [&] { return std::ranges::count(lines, "greeter: hello Alex on a of 2 online 1") > 0; }));
}

TEST_CASE("plugin data persists across restarts and stays private to each plugin") {
    const std::filesystem::path root = std::filesystem::path{BASALT_TEST_SCRATCH_DIR} / "data";
    std::filesystem::remove_all(root);
    manifest(root / "plugins" / "counter", "counter", "[capabilities]\nstorage = true\n");
    write(root / "plugins" / "counter" / "src" / "main.lua", "on('player.join', function(e)\n"
                                                             "  local n = (store.visits or 0) + 1\n"
                                                             "  store.visits = n\n"
                                                             "  log.info('visits ' .. n)\n"
                                                             "end)\n");
    manifest(root / "plugins" / "snoop", "snoop", "[capabilities]\nstorage = true\n");
    write(root / "plugins" / "snoop" / "src" / "main.lua",
          "on('player.join', function(e) log.info('snoop sees ' .. tostring(store.visits)) end)\n");

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
        (*host)->post({runtime::PlayerEventKind::kJoin, "Steve", "a", "uuid-1", {}});
        std::unique_lock lock{mutex};
        REQUIRE(ready.wait_for(lock, 5s, [&] { return std::ranges::count(lines, "snoop: snoop sees nil") == run; }));
    }
    std::lock_guard lock{mutex};
    CHECK(std::ranges::count(lines, "counter: visits 1") == 1);
    CHECK(std::ranges::count(lines, "counter: visits 2") == 1);
}

TEST_CASE("packet listeners written in Lua read, edit, cancel and send, and a broken one is contained") {
    using namespace basalt::wire::plugins;
    const std::filesystem::path root = std::filesystem::path{BASALT_TEST_SCRATCH_DIR} / "packet-plugins";
    std::filesystem::remove_all(root);
    manifest(root / "tap", "tap", "[capabilities]\npackets = true\n");
    write(root / "tap" / "src" / "main.lua",
          "packet.on('clientbound.play.*', function(p)\n"
          "  if p.id == 5 then p.bytes = {p.bytes[1] + 1, 9} elseif p.id == 6 then p:cancel() end\n"
          "  if p.id == 7 then p:send('clientbound.play.0x08', {1, 2}) end\n"
          "end)\n"
          "packet.watch('*', function(p) p.cancelled = true end)\n");
    manifest(root / "loop", "loop", "[capabilities]\npackets = true\n");
    write(root / "loop" / "src" / "main.lua", "packet.on('serverbound.*', function() while true do end end)\n");
    plugins::PluginSettings settings;
    settings.directory = root.string();
    settings.limits.instructionsPerTurn = 20000;
    settings.allow = {{"tap", {"packets"}}, {"loop", {"packets"}}};
    std::atomic<plugins::PluginHost*> hostPointer{nullptr};
    auto replaced = std::make_shared<PacketRegistry>(
        [&hostPointer](const void* owner, const basalt::wire::lua::LuaRef& fn, PluginPacket& packet) {
            return hostPointer.load()->deliverPacket(owner, fn, packet);
        },
        [](const PacketAddress& address) { return packetNaming::of(address); });
    settings.packets = replaced;
    auto host = plugins::PluginHost::start(settings, {});
    REQUIRE(host);
    hostPointer.store(host->get());
    CHECK((*host)->loaded() == 2);

    struct Endpoint final : PacketEndpoint {
        std::vector<std::int32_t> sent;

        bool sendClientbound(std::int32_t id, std::span<const std::uint8_t>, bool) override {
            sent.push_back(id);
            return true;
        }

        bool receiveServerbound(std::int32_t, std::span<const std::uint8_t>, bool) override { return true; }

        void close() override {}

        std::uint64_t connectionId() const override { return 1; }

        std::string address() const override { return {}; }

        std::uint16_t port() const override { return 0; }
    };

    const auto endpoint = std::make_shared<Endpoint>();
    PluginPacket packet;
    packet.endpoint = endpoint;
    packet.address = {PacketPhase::kPlay, false, 5};
    packet.bytes = {4};
    CHECK(replaced->fire(packet));
    CHECK(packet.replacement == std::optional{std::vector<std::uint8_t>{5, 9}});
    packet.address.id = 6;
    CHECK_FALSE(replaced->fire(packet));
    packet.address.id = 7;
    CHECK(replaced->fire(packet));
    CHECK(endpoint->sent == std::vector<std::int32_t>{8});
    packet.address = {PacketPhase::kPlay, true, 1};
    for (int i = 0; i < 8; ++i)
        CHECK(replaced->fire(packet));
    packet.address = {PacketPhase::kPlay, false, 5};
    CHECK(replaced->fire(packet));
    CHECK(packet.replacement.has_value());
}

TEST_CASE("decision events let handlers deny or redirect, and nothing listening means no wait") {
    const std::filesystem::path root = std::filesystem::path{BASALT_TEST_SCRATCH_DIR} / "plugins-decisions";
    std::filesystem::remove_all(root);
    manifest(root / "gate", "gate");
    write(
        root / "gate" / "src" / "main.lua",
        "on('player.login', function(e) if e.player_name == 'Bad' then e.cancelled = true e.reason = 'nope' end end)\n"
        "on('player.server_pre_connect', {priority = 'first'}, function(e) wait(1) if e.server == 'a' then "
        "e.server = 'b' end end)\n"
        "on('player.kick', function(e) e.server = 'lobby' end)\n"
        "on('player.command', function(e) if e.command == 'bad' then e.cancelled = true else e.command = "
        "e.command .. '!' end end)\n"
        "on('server.ping', function(e) e.motd = 'hi ' .. e.protocol e.online = 3 end)\n");

    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::pair<std::uint64_t, runtime::DecisionResult>> answers;
    plugins::PluginHooks hooks;
    hooks.resolveDecision = [&](std::string_view, std::uint64_t token, runtime::DecisionResult result) {
        std::lock_guard lock{mutex};
        answers.emplace_back(token, std::move(result));
        ready.notify_all();
    };
    plugins::PluginSettings settings;
    settings.directory = root.string();
    auto host = plugins::PluginHost::start(settings, hooks);
    REQUIRE(host);
    const auto ask = [&](std::uint64_t token, runtime::DecisionKind kind, std::string player, std::string backend) {
        runtime::DecisionRequest request;
        request.token = token;
        request.kind = kind;
        request.player = std::move(player);
        request.backend = backend;
        request.text = std::move(backend);
        request.deadlineMs = 3000;
        return (*host)->decide(std::move(request));
    };
    CHECK(ask(1, runtime::DecisionKind::kLogin, "Bad", {}));
    CHECK(ask(2, runtime::DecisionKind::kPreConnect, "Ann", "a"));
    CHECK(ask(3, runtime::DecisionKind::kKick, "Ann", {}));
    CHECK(ask(4, runtime::DecisionKind::kCommand, "Ann", "bad"));
    CHECK(ask(5, runtime::DecisionKind::kCommand, "Ann", "home"));
    CHECK(ask(6, runtime::DecisionKind::kPing, {}, {}));
    std::unique_lock lock{mutex};
    REQUIRE(ready.wait_for(lock, 5s, [&] { return answers.size() == 6; }));
    std::ranges::sort(answers, {}, &decltype(answers)::value_type::first);
    CHECK(answers[0].second.deny);
    CHECK(answers[0].second.reason == "nope");
    CHECK_FALSE(answers[1].second.deny);
    CHECK(answers[1].second.redirect == "b");
    CHECK(answers[2].second.redirect == "lobby");
    CHECK(answers[3].second.deny);
    CHECK(answers[4].second.text == "home!");
    CHECK(answers[5].second.online == 3);
}

#if defined(BASALT_HAS_SHIM)
#endif
