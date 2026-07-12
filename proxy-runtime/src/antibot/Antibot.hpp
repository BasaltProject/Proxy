#pragma once

#include <basalt/proxy/config/AntibotConfig.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace basalt::proxy::runtime::antibot {

using Clock = std::chrono::steady_clock;

/// An address reduced to what one origin is: the whole of an IPv4 address, the /64 of an IPv6 one.
struct OriginKey {
    std::array<std::uint8_t, 16> bytes{};

    friend bool operator==(const OriginKey&, const OriginKey&) = default;
};

struct OriginHash {
    [[nodiscard]] std::size_t operator()(const OriginKey& key) const noexcept;
};

[[nodiscard]] OriginKey originOf(std::string_view literal);

enum class Action : std::uint8_t { kAllow, kLimbo, kCaptcha, kDeny };

enum class Reason : std::uint8_t {
    kNone,
    kTrusted,
    kVerified,
    kBlocked,
    kBanned,
    kHost,
    kNamePattern,
    kNoPing,
    kAttack,
    kScore,
    kLimboFull,
    kLegacy,
    kLimboFailed,
    kCaptchaFailed,
    kTimeout,
    kPassthrough,
    kProtocol,
    kProxy,
    kExploit
};

[[nodiscard]] std::string_view reasonName(Reason reason) noexcept;
[[nodiscard]] std::string_view actionName(Action action) noexcept;

struct Verdict {
    Action action = Action::kAllow;
    Reason reason = Reason::kNone;
    std::int32_t score = 0;
};

struct Event {
    enum class Kind : std::uint8_t { kVerdict, kAttackStarted, kAttackEnded, kVerified, kLimboFailed, kBanned };

    Kind kind = Kind::kVerdict;
    std::string origin;
    std::string player;
    Verdict verdict;
    std::uint32_t skipped = 0;
};

struct Stats {
    std::uint64_t connections = 0;
    std::uint64_t denied = 0;
    std::uint64_t limboEntered = 0;
    std::uint64_t limboPassed = 0;
    std::uint64_t limboFailed = 0;
    std::uint64_t attacks = 0;
    std::uint64_t exploits = 0;
    std::uint32_t limboNow = 0;
    bool attackActive = false;
    std::size_t origins = 0;
    std::size_t verified = 0;
};

struct Settings;
class Antibot;

/// Holds one place in the limbo, for the whole of its owner's life.
class LimboSlot {
public:
    LimboSlot() = default;

    LimboSlot(Antibot* owner, OriginKey origin, std::string name)
        : owner_(owner), origin_(origin), name_(std::move(name)) {}

    LimboSlot(const LimboSlot&) = delete;
    LimboSlot& operator=(const LimboSlot&) = delete;

    LimboSlot(LimboSlot&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), origin_(other.origin_), name_(std::move(other.name_)) {}

    LimboSlot& operator=(LimboSlot&& other) noexcept;
    ~LimboSlot();

private:
    Antibot* owner_ = nullptr;
    OriginKey origin_;
    std::string name_;
};

/// Where verified players are kept across restarts; the shared players database plugs in here.
struct VerifiedStore {
    std::function<void(std::string_view name, std::string_view origin, std::int64_t expiresUnixSeconds)> save;
    std::function<void(std::string_view name)> erase;
};

/// Everything antibot decides, shared by every session owner. Safe to call from any thread.
class Antibot {
public:
    using Sink = std::function<void(const Event&)>;

    explicit Antibot(const config::AntibotConfig& config, Clock::time_point start = Clock::now());
    ~Antibot();
    Antibot(const Antibot&) = delete;
    Antibot& operator=(const Antibot&) = delete;

    /// Checks the new settings whole, then swaps them in; the old ones stay on an error.
    [[nodiscard]] std::expected<void, std::string> apply(const config::AntibotConfig& config);
    void observe(Sink sink);
    void persistVerified(VerifiedStore store);
    /// True when the text matches an exploit pattern; only the first 512 bytes are searched.
    [[nodiscard]] bool exploitText(std::string_view text) const;
    void penalize(const OriginKey& origin, std::int32_t points, Clock::time_point now);
    void exploitRefused(const OriginKey& origin, Clock::time_point now);
    /// Loads a stored verified entry; an empty origin trusts the name from anywhere.
    void restoreVerified(std::string_view name, std::string_view origin, std::int64_t expiresUnixSeconds);

    [[nodiscard]] std::shared_ptr<const Settings> settings() const;
    [[nodiscard]] std::shared_ptr<const config::AntibotConfig> config() const;
    [[nodiscard]] bool enabled() const;

    [[nodiscard]] Verdict connection(const OriginKey& origin, Clock::time_point now);
    [[nodiscard]] Verdict handshake(const OriginKey& origin, std::string_view host, std::int32_t protocol,
                                    Clock::time_point now);
    /// False when this origin asks for the server list too fast; the request is then ignored.
    [[nodiscard]] bool statusAllowed(const OriginKey& origin, Clock::time_point now);
    [[nodiscard]] Verdict login(const OriginKey& origin, std::string_view name, bool legacyClient,
                                Clock::time_point now);
    void loginFailed(const OriginKey& origin, Clock::time_point now);

    [[nodiscard]] std::optional<LimboSlot> enterLimbo(const OriginKey& origin, std::string_view name,
                                                      Clock::time_point now);
    void limboPassed(const OriginKey& origin, std::string_view name, Clock::time_point now);
    void limboFailed(const OriginKey& origin, std::string_view name, Reason reason, Clock::time_point now);

    /// Trusts `name` from `origin`, or from anywhere when `origin` is empty, until the verified lifetime ends.
    void verify(std::string_view name, const std::optional<OriginKey>& origin, Clock::time_point now);
    void unverify(std::string_view name);
    [[nodiscard]] bool isVerified(std::string_view name, const std::optional<OriginKey>& origin,
                                  Clock::time_point now) const;

    [[nodiscard]] bool attackActive(Clock::time_point now);
    /// 1 forces attack mode on, -1 forces it off, 0 returns to the automatic decision.
    void forceAttack(int mode);
    void ban(const OriginKey& origin, Clock::time_point now);
    bool unban(const OriginKey& origin);
    [[nodiscard]] Stats stats(Clock::time_point now);

private:
    friend class LimboSlot;
    struct Impl;
    void leaveLimbo(const OriginKey& origin, const std::string& name);
    std::unique_ptr<Impl> impl_;
};

} // namespace basalt::proxy::runtime::antibot
