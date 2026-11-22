#pragma once

#include "PluginInternal.hpp"
#include "WasmWatchdog.hpp"

#include <basalt/wire/common/toml/Toml.hpp>
#include <basalt/wire/plugins/CommandArguments.hpp>
#include <basalt/wire/plugins/PluginText.hpp>
#include <basalt/wire/wasm/Instance.hpp>
#include <basalt/wire/wasm/Value.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace basalt::proxy::plugins {

enum class WasmEvent : std::uint8_t {
    kEnable,
    kDisable,
    kJoin,
    kServerChange,
    kQuit,
    kLogin,
    kPreConnect,
    kPing,
    kChat,
    kCommand,
    kKick
};
inline constexpr std::size_t kWasmEventCount = 11;

[[nodiscard]] std::optional<WasmEvent> wasmEventOf(std::string_view name);

/// What a decided event lets a handler read and change; PluginHost copies it to and from its decision.
struct WasmDecision {
    enum Touched : unsigned {
        kReason = 1U,
        kServer = 2U,
        kMotd = 4U,
        kVersion = 8U,
        kSample = 16U,
        kFavicon = 32U,
        kMaxPlayers = 64U,
        kOnline = 128U,
        kVersionProtocol = 256U,
        kText = 512U,
        kReasonJson = 1024U,
        kMotdJson = 2048U
    };

    runtime::DecisionKind kind = runtime::DecisionKind::kLogin;
    std::string_view player;
    std::string_view uuid;
    std::string_view address;
    std::uint16_t port = 0;
    std::string_view host;
    std::string_view from;
    std::string_view protocol;
    bool cancelled = false;
    bool signedContent = false;
    bool reasonJson = false;
    bool motdJson = false;
    std::string text;
    std::string reason;
    std::string server;
    std::string motd;
    std::string version;
    std::string sample;
    std::string favicon;
    std::int64_t maxPlayers = -1;
    std::int64_t online = -1;
    std::int64_t versionProtocol = -1;
    unsigned touched = 0;
};

struct WasmPlugin;

/// The callback in progress: what its context handle answers.
struct WasmCall {
    std::uint32_t id = 0;
    WasmEvent event = WasmEvent::kEnable;
    bool observed = false;
    std::string_view text;
    std::string_view name;
    std::string_view uuid;
    std::string_view address;
    std::string_view server;
    std::string_view from;
    std::int64_t port = 0;
    WasmDecision* decision = nullptr;
    bool command = false;
    bool console = false;
    const wire::plugins::CommandSpec* spec = nullptr;
    const std::vector<wire::plugins::CommandValue>* values = nullptr;
    const wire::wasm::Value* value = nullptr;
};

struct WasmTimer {
    WasmPlugin* plugin = nullptr;
    std::int32_t id = 0;
    std::int32_t token = 0;
    std::chrono::steady_clock::time_point due;
    std::chrono::milliseconds period{0};
    /// A request handle: when due, its completion runs with a null value instead of a plain callback.
    std::int32_t request = 0;
};

struct WasmCommand {
    WasmPlugin* plugin = nullptr;
    wire::plugins::CommandSpec spec;
    std::int32_t token = 0;
};

/// Runs the WebAssembly plugins on the plugin thread, on the plugin thread.
class WasmHost {
public:
    using Player = PluginHost::Player;

    explicit WasmHost(PluginHost& host);
    ~WasmHost();
    WasmHost(const WasmHost&) = delete;
    WasmHost& operator=(const WasmHost&) = delete;

    [[nodiscard]] bool ready() const noexcept { return engine_.has_value(); }

    /// False (after logging why) when the plugin could not start.
    bool load(const std::shared_ptr<wire::plugins::PluginPackage>& package,
              const wire::plugins::PluginManifest& manifest);

    void enable();
    void disable();

    void observe(std::string_view event, std::string_view name, std::string_view uuid, std::string_view server,
                 std::string_view address, std::uint16_t port, std::string_view from);
    void forget(std::string_view player);
    void decide(WasmDecision& decision);

    [[nodiscard]] bool hasRoot(std::string_view root) const;
    /// True when a WebAssembly plugin owns the command; the source is null for the console.
    bool invoke(const std::vector<std::string>& words, const PluginHost::Player* source);

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> nextDue() const;
    void runTimers(std::chrono::steady_clock::time_point now);

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> listing() const;

    void logLine(const WasmPlugin& plugin, std::string_view prefix, std::string_view line) const;

    [[nodiscard]] const PluginHooks& hooks() const noexcept { return host_.hooks_; }

    [[nodiscard]] PluginHost& host() noexcept { return host_; }

    /// Whether a plugin callback is running, so a provider cannot be called from inside it.
    [[nodiscard]] bool busy() const noexcept { return running_; }

    [[nodiscard]] std::shared_mutex& commandsMutex() const noexcept { return host_.commandsMutex_; }

    void addDecider(unsigned bit) { host_.deciders_.fetch_or(bit); }

    [[nodiscard]] const auto& roster() const noexcept { return host_.roster_; }

    [[nodiscard]] wire::sql::Database* database() { return host_.database_ ? &*host_.database_ : nullptr; }

    bool dispatch(WasmPlugin& plugin, std::int32_t token, WasmCall* call, std::string_view why,
                  std::chrono::milliseconds budget);
    void fail(WasmPlugin& plugin, const wire::wasm::Failure& failure, std::string_view why);
    void notify(WasmPlugin& plugin, WasmEvent event, WasmCall* call, bool watchers);
    [[nodiscard]] WasmCall* live(std::int32_t ctx) noexcept;
    /// Packet listeners run on session owners: they take the turn, so no instance is entered by two threads.
    bool deliverPacket(const void* owner, std::int64_t token, wire::plugins::PluginPacket& packet);
    void deliverMessage(std::string_view player, std::string_view channel, std::span<const std::uint8_t> data,
                        bool fromBackend);
    void antibotEvent(const runtime::PlayerEvent& event);
    void dropPacketListeners();
    void announceChannel(std::string_view channel, bool wanted);

    [[nodiscard]] wire::plugins::PacketRegistry* packetRegistry() const noexcept {
        return host_.settings_.packets.get();
    }

    [[nodiscard]] std::size_t packetMaxBytes() const noexcept { return host_.settings_.packetMaxBytes; }

    [[nodiscard]] std::int32_t handleOf(std::string_view player);
    [[nodiscard]] const PluginHost::Player* playerOf(std::int32_t handle);

    PluginHost& host_;
    std::optional<wire::wasm::Engine> engine_;
    WasmWatchdog watchdog_;
    std::vector<std::unique_ptr<WasmPlugin>> plugins_;
    std::map<std::string, std::vector<WasmCommand>, std::less<>> commands_;
    std::vector<WasmTimer> timers_;
    std::vector<std::int32_t> dueScratch_;
    std::map<std::string, std::int32_t, std::less<>> handles_;
    std::map<std::int32_t, std::string> names_;
    std::int32_t nextHandle_ = 1;
    std::int32_t nextTimer_ = 1;
    std::uint32_t nextCall_ = 1;
    WasmCall* current_ = nullptr;
    bool running_ = false;
    std::recursive_timed_mutex turn_;
    std::atomic<std::thread::id> turnHolder_{};
};

/// Holds the turn for the scope and records which thread has it, so a nested delivery can tell.
class WasmTurn {
public:
    explicit WasmTurn(WasmHost& host) : host_(host), lock_(host.turn_), previous_(host.turnHolder_.load()) {
        host.turnHolder_.store(std::this_thread::get_id());
    }

    ~WasmTurn() { host_.turnHolder_.store(previous_); }

    WasmTurn(const WasmTurn&) = delete;
    WasmTurn& operator=(const WasmTurn&) = delete;

private:
    WasmHost& host_;
    std::lock_guard<std::recursive_timed_mutex> lock_;
    std::thread::id previous_;
};

} // namespace basalt::proxy::plugins
