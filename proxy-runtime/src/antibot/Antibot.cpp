#include "Antibot.hpp"

#include "AddressFeeds.hpp"
#include "TextScan.hpp"
#include "VerifiedKeeper.hpp"

#include <basalt/proxy/config/AddressRange.hpp>
#include <basalt/proxy/config/PatternSafety.hpp>

#include <algorithm>
#include <cctype>
#include <list>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace basalt::proxy::runtime::antibot {
namespace {
constexpr std::size_t kShards = 16;
constexpr std::int64_t kDecayMs = 6000;
constexpr std::size_t kNameMemory = 8;

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

[[nodiscard]] std::uint64_t hashOf(std::string_view text, bool skeleton) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(raw)));
        if (skeleton && (std::isdigit(c) || c == '_'))
            continue;
        hash = (hash ^ c) * 0x100000001b3ULL;
    }
    return mix(hash);
}

[[nodiscard]] std::uint64_t hashOf(const OriginKey& key) noexcept {
    std::uint64_t a = 0, b = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        a = (a << 8) | key.bytes[i];
        b = (b << 8) | key.bytes[8 + i];
    }
    return mix(a ^ mix(b));
}

[[nodiscard]] std::string lowered(std::string_view text) {
    std::string result{text};
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

[[nodiscard]] bool isV4(const OriginKey& key) {
    const auto& b = key.bytes;
    return std::all_of(b.begin(), b.begin() + 10, [](auto v) { return v == 0; }) && b[10] == 0xff && b[11] == 0xff;
}

[[nodiscard]] std::string storable(const OriginKey& key) {
    char text[64];
    const auto& b = key.bytes;
    if (isV4(key))
        std::snprintf(text, sizeof text, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
    else
        std::snprintf(text, sizeof text, "%x:%x:%x:%x::", b[0] << 8 | b[1], b[2] << 8 | b[3], b[4] << 8 | b[5],
                      b[6] << 8 | b[7]);
    return text;
}

[[nodiscard]] std::string printable(const OriginKey& key) {
    char text[64];
    const auto& b = key.bytes;
    const bool v4 =
        std::all_of(b.begin(), b.begin() + 10, [](auto v) { return v == 0; }) && b[10] == 0xff && b[11] == 0xff;
    if (v4)
        std::snprintf(text, sizeof text, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
    else
        std::snprintf(text, sizeof text, "%02x%02x:%02x%02x:%02x%02x:%02x%02x::/64", b[0], b[1], b[2], b[3], b[4], b[5],
                      b[6], b[7]);
    return text;
}
} // namespace

std::size_t OriginHash::operator()(const OriginKey& key) const noexcept {
    return static_cast<std::size_t>(hashOf(key));
}

OriginKey originOf(std::string_view literal) {
    OriginKey key;
    if (const auto bytes = config::AddressRange::parseAddress(literal)) {
        key.bytes = *bytes;
        const bool v4 = std::all_of(bytes->begin(), bytes->begin() + 10, [](auto v) { return v == 0; }) &&
                        (*bytes)[10] == 0xff && (*bytes)[11] == 0xff;
        if (!v4)
            std::fill(key.bytes.begin() + 8, key.bytes.end(), std::uint8_t{0});
    } else {
        const auto hash = hashOf(literal, false);
        for (std::size_t i = 0; i < 8; ++i)
            key.bytes[i] = static_cast<std::uint8_t>(hash >> (i * 8));
        key.bytes[15] = 0xee;
    }
    return key;
}

std::string_view reasonName(Reason reason) noexcept {
    switch (reason) {
    case Reason::kNone:
        return "none";
    case Reason::kTrusted:
        return "trusted";
    case Reason::kVerified:
        return "verified";
    case Reason::kBlocked:
        return "blocked";
    case Reason::kBanned:
        return "banned";
    case Reason::kHost:
        return "host";
    case Reason::kNamePattern:
        return "name";
    case Reason::kNoPing:
        return "no-ping";
    case Reason::kAttack:
        return "attack";
    case Reason::kScore:
        return "score";
    case Reason::kLimboFull:
        return "limbo-full";
    case Reason::kLegacy:
        return "legacy-client";
    case Reason::kLimboFailed:
        return "limbo-failed";
    case Reason::kCaptchaFailed:
        return "captcha-failed";
    case Reason::kPassthrough:
        return "passthrough";
    case Reason::kProtocol:
        return "protocol";
    case Reason::kProxy:
        return "proxy";
    case Reason::kExploit:
        return "exploit";
    case Reason::kBusy:
        return "busy";
    case Reason::kTimeout:
        return "timeout";
    }
    return "none";
}

std::string_view actionName(Action action) noexcept {
    switch (action) {
    case Action::kAllow:
        return "allow";
    case Action::kLimbo:
        return "limbo";
    case Action::kCaptcha:
        return "captcha";
    case Action::kDeny:
        return "deny";
    case Action::kChatCheck:
        return "chat-check";
    }
    return "allow";
}

struct Settings {
    config::AntibotConfig config;
    std::vector<config::AddressRange> trusted;
    std::vector<config::AddressRange> blocked;
    std::vector<config::AddressRange> geo;
    std::vector<std::regex> patterns;
    TextScan exploits;
    std::unordered_set<std::string> hosts;

    [[nodiscard]] static std::expected<std::shared_ptr<const Settings>, std::string>
    make(const config::AntibotConfig& config) {
        auto result = std::make_shared<Settings>();
        result->config = config;
        for (const auto& entry : config.trusted)
            if (auto range = config::AddressRange::parse(entry))
                result->trusted.push_back(*range);
        for (const auto& entry : config.blocked)
            if (auto range = config::AddressRange::parse(entry))
                result->blocked.push_back(*range);
        for (const auto& entry : config.geoRanges)
            if (auto range = config::AddressRange::parse(entry))
                result->geo.push_back(*range);
        try {
            for (const auto& pattern : config.blockedNamePatterns)
                result->patterns.emplace_back(pattern);
        } catch (const std::regex_error&) {
            return std::unexpected("a blocked name pattern is not a valid regular expression");
        }
        for (const auto& host : config.allowedHosts) {
            auto name = lowered(host);
            while (name.ends_with('.'))
                name.pop_back();
            result->hosts.insert(std::move(name));
        }
        result->exploits = TextScan{config.exploitPatterns};
        return std::shared_ptr<const Settings>{std::move(result)};
    }

    [[nodiscard]] static bool within(const std::vector<config::AddressRange>& ranges, const OriginKey& origin) {
        return std::ranges::any_of(ranges, [&](const auto& range) { return range.contains(origin.bytes); });
    }
};

namespace {
struct Record {
    Clock::time_point lastSeen{};
    Clock::time_point scoredAt{};
    Clock::time_point bannedUntil{};
    Clock::time_point pingedAt{};
    Clock::time_point statusWindow{};
    std::int32_t score = 0;
    std::uint16_t statusCount = 0;
    std::uint16_t limbo = 0;
    std::uint8_t nameCount = 0;
    bool crossed = false;
    std::array<std::uint64_t, kNameMemory> names{};
    std::array<std::uint64_t, kNameMemory> skeletons{};

    [[nodiscard]] std::int32_t effective(Clock::time_point now) const {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - scoredAt).count();
        return std::max<std::int32_t>(0,
                                      score - static_cast<std::int32_t>(std::max<std::int64_t>(0, elapsed) / kDecayMs));
    }

    void add(std::int32_t points, Clock::time_point now) {
        score = static_cast<std::int32_t>(
            std::clamp<std::int64_t>(std::int64_t{effective(now)} + points, INT32_MIN, INT32_MAX));
        scoredAt = now;
    }
};

struct Shard {
    std::mutex mutex;
    std::unordered_map<OriginKey, Record, OriginHash> records;
};
} // namespace

struct Antibot::Impl {
    explicit Impl(std::shared_ptr<const Settings> initial, Clock::time_point start)
        : current(std::move(initial)), epoch(start) {}

    mutable std::mutex settingsMutex;
    std::shared_ptr<const Settings> current;
    Sink sink;
    Clock::time_point epoch;
    std::array<Shard, kShards> shards;

    std::atomic<std::uint64_t> connections{0}, denied{0}, limboEntered{0}, limboPassed{0}, limboFailed{0}, attacks{0};
    std::atomic<std::uint32_t> limboNow{0};
    std::unique_ptr<AddressFeeds> feeds;

    std::mutex verifiedMutex;
    std::unordered_map<std::uint64_t, VerifiedEntry> verified;
    VerifiedStore store;
    std::unique_ptr<VerifiedKeeper> keeper;
    std::atomic<std::uint64_t> exploitsRefused{0};
    std::mutex limboNamesMutex;
    std::unordered_set<std::string> limboNames;
    std::mutex passthroughMutex;
    std::unordered_map<std::uint64_t, Clock::time_point> passthroughPending;
    std::unordered_map<std::uint64_t, Clock::time_point> transfers;

    std::atomic<std::int64_t> second{-1};
    std::atomic<bool> attack{false};
    std::atomic<std::int64_t> attackStart{-1};
    std::atomic<std::int64_t> entrySecond{-1};
    std::atomic<std::int32_t> entryCount{0};
    std::unordered_map<std::uint64_t, Clock::time_point> knownOrigins;
    std::atomic<int> manual{0};
    std::atomic<bool> hard{false};

    [[nodiscard]] bool attackNow() const {
        const auto forced = manual.load(std::memory_order_relaxed);
        return forced == 0 ? attack.load(std::memory_order_relaxed) : forced > 0;
    }

    std::mutex attackMutex;
    std::atomic<std::uint32_t> perSecondConnections{0}, perSecondFresh{0}, perSecondFailures{0};
    std::uint32_t overFor = 0, calmFor = 0;

    [[nodiscard]] std::shared_ptr<const Settings> snapshot() const {
        const std::lock_guard lock{settingsMutex};
        return current;
    }

    [[nodiscard]] Shard& shardOf(const OriginKey& key) { return shards[hashOf(key) % kShards]; }

    static constexpr std::uint32_t kVerdictEventsPerSecond = 20;
    std::atomic<std::int64_t> verdictSecond{-1};
    std::atomic<std::uint32_t> verdictCount{0};
    std::atomic<std::uint32_t> verdictSkipped{0};

    void emit(Event::Kind kind, const OriginKey& origin, std::string_view player, Verdict verdict) {
        if (kind == Event::Kind::kAttackStarted && keeper)
            keeper->poke();
        std::uint32_t skipped = 0;
        if (kind == Event::Kind::kVerdict) {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - epoch).count();
            if (verdictSecond.exchange(now, std::memory_order_relaxed) != now) {
                skipped = verdictSkipped.exchange(0, std::memory_order_relaxed);
                verdictCount.store(0, std::memory_order_relaxed);
            }
            if (verdictCount.fetch_add(1, std::memory_order_relaxed) >= kVerdictEventsPerSecond) {
                verdictSkipped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        Sink local;
        {
            const std::lock_guard lock{settingsMutex};
            local = sink;
        }
        if (local)
            local(Event{kind, printable(origin), std::string{player}, verdict, skipped});
    }

    Record& recordOf(Shard& shard, const OriginKey& key, const Settings& settings, Clock::time_point now, bool& fresh) {
        const auto found = shard.records.find(key);
        const auto expiry = std::chrono::seconds{settings.config.originExpirySeconds};
        if (found != shard.records.end() && now - found->second.lastSeen <= expiry) {
            found->second.lastSeen = now;
            fresh = false;
            return found->second;
        }
        if (found != shard.records.end())
            shard.records.erase(found);
        const auto capacity = std::max<std::size_t>(1, settings.config.maxOrigins / kShards);
        if (shard.records.size() >= capacity) {
            auto oldest = shard.records.begin();
            auto it = oldest;
            for (std::size_t i = 0; i < 16 && it != shard.records.end(); ++i, ++it)
                if (it->second.lastSeen < oldest->second.lastSeen)
                    oldest = it;
            shard.records.erase(oldest);
        }
        fresh = true;
        auto& record = shard.records[key];
        record.lastSeen = now;
        record.scoredAt = now;
        return record;
    }

    bool passthroughReturned(std::uint64_t key, Clock::time_point now, const config::AntibotConfig& c, bool consume) {
        const std::lock_guard lock{passthroughMutex};
        const auto found = passthroughPending.find(key);
        if (found == passthroughPending.end())
            return false;
        const bool inWindow = now - found->second <= std::chrono::seconds{c.passthroughWindowSeconds};
        if (consume || !inWindow)
            passthroughPending.erase(found);
        return inWindow;
    }

    [[nodiscard]] bool takeEntry(const config::AntibotConfig& c, Clock::time_point now) {
        if (c.attackEntriesPerSecond <= 0 || !attackNow())
            return true;
        const auto secondNow = std::chrono::duration_cast<std::chrono::seconds>(now - epoch).count();
        if (entrySecond.exchange(secondNow, std::memory_order_relaxed) != secondNow)
            entryCount.store(0, std::memory_order_relaxed);
        return entryCount.fetch_add(1, std::memory_order_relaxed) < c.attackEntriesPerSecond;
    }

    void passthroughSend(std::uint64_t key, Clock::time_point now, const config::AntibotConfig& c) {
        const std::lock_guard lock{passthroughMutex};
        if (passthroughPending.size() >= c.verifiedMax)
            passthroughPending.clear();
        passthroughPending[key] = now;
    }

    [[nodiscard]] bool passthroughOn(const config::AntibotConfig& c, bool attackOn, Clock::time_point now) const {
        if (c.passthroughEnabled && (c.passthroughAlways || attackOn))
            return true;
        if (!attackOn || c.attackPassthroughAfterSeconds == 0)
            return false;
        const auto start = attackStart.load(std::memory_order_relaxed);
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - epoch).count();
        return start >= 0 && seconds - start >= static_cast<std::int64_t>(c.attackPassthroughAfterSeconds);
    }

    void markAttackStart(Clock::time_point now) {
        attackStart.store(std::chrono::duration_cast<std::chrono::seconds>(now - epoch).count(),
                          std::memory_order_relaxed);
    }

    void surge(const Settings& settings, Clock::time_point now) {
        const auto& c = settings.config;
        if (!c.attackEnabled || attack.load(std::memory_order_relaxed))
            return;
        const auto conn = static_cast<std::int64_t>(perSecondConnections.load(std::memory_order_relaxed));
        const auto fresh = static_cast<std::int64_t>(perSecondFresh.load(std::memory_order_relaxed));
        if (conn < 2 * c.attackConnectionsPerSecond && fresh < 2 * c.attackNewOriginsPerSecond)
            return;
        {
            const std::lock_guard lock{attackMutex};
            if (attack.exchange(true, std::memory_order_relaxed))
                return;
            calmFor = 0;
            markAttackStart(now);
            attacks.fetch_add(1, std::memory_order_relaxed);
        }
        emit(Event::Kind::kAttackStarted, OriginKey{}, {}, Verdict{});
    }

    void advance(const Settings& settings, Clock::time_point now) {
        const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(now - epoch).count();
        if (now_s == second.load(std::memory_order_relaxed))
            return;
        bool started = false, ended = false;
        {
            const std::lock_guard lock{attackMutex};
            auto last = second.load(std::memory_order_relaxed);
            if (now_s <= last)
                return;
            const auto gap =
                last < 0 ? 0 : std::min<std::int64_t>(now_s - last - 1, settings.config.attackCooldownSeconds + 1);
            const auto& c = settings.config;
            const auto conn = perSecondConnections.exchange(0), fresh = perSecondFresh.exchange(0),
                       fail = perSecondFailures.exchange(0);
            const auto over = static_cast<std::int64_t>(conn) >= c.attackConnectionsPerSecond ||
                              static_cast<std::int64_t>(fresh) >= c.attackNewOriginsPerSecond ||
                              static_cast<std::int64_t>(fail) >= c.attackFailedLoginsPerSecond;
            const auto calm = static_cast<std::int64_t>(conn) * 2 < c.attackConnectionsPerSecond &&
                              static_cast<std::int64_t>(fresh) * 2 < c.attackNewOriginsPerSecond &&
                              static_cast<std::int64_t>(fail) * 2 < c.attackFailedLoginsPerSecond;
            second.store(now_s, std::memory_order_relaxed);
            if (c.attackHardConnectionsPerSecond > 0) {
                const auto limit = static_cast<std::int64_t>(c.attackHardConnectionsPerSecond);
                if (static_cast<std::int64_t>(conn) >= limit)
                    hard.store(true, std::memory_order_relaxed);
                else if (static_cast<std::int64_t>(conn) * 2 < limit)
                    hard.store(false, std::memory_order_relaxed);
            }
            if (!c.attackEnabled) {
                overFor = calmFor = 0;
                ended = attack.exchange(false);
            } else if (!attack.load(std::memory_order_relaxed)) {
                overFor = over ? overFor + 1 : 0;
                if (overFor >= c.attackConfirmSeconds) {
                    attack.store(true, std::memory_order_relaxed);
                    calmFor = 0;
                    markAttackStart(now);
                    started = true;
                    attacks.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                calmFor = (calm ? calmFor + 1 : 0) + static_cast<std::uint32_t>(gap);
                if (calmFor >= c.attackCooldownSeconds) {
                    attack.store(false, std::memory_order_relaxed);
                    hard.store(false, std::memory_order_relaxed);
                    overFor = 0;
                    ended = true;
                }
            }
        }
        if (started)
            emit(Event::Kind::kAttackStarted, OriginKey{}, {}, Verdict{});
        if (ended)
            emit(Event::Kind::kAttackEnded, OriginKey{}, {}, Verdict{});
    }

    [[nodiscard]] static std::uint64_t verifiedKey(std::string_view name, const std::optional<OriginKey>& origin) {
        return mix(hashOf(name, false) ^ (origin ? hashOf(*origin) | 1 : 0));
    }

    [[nodiscard]] bool verifiedNow(const Settings& settings, std::string_view name, const OriginKey& origin,
                                   Clock::time_point now) {
        if (settings.config.verifiedTtlHours == 0)
            return false;
        const std::lock_guard lock{verifiedMutex};
        for (const auto& key : {verifiedKey(name, origin), verifiedKey(name, std::nullopt)}) {
            const auto found = verified.find(key);
            if (found == verified.end())
                continue;
            if (found->second.expiry > now)
                return true;
            verified.erase(found);
        }
        return false;
    }
};

LimboSlot& LimboSlot::operator=(LimboSlot&& other) noexcept {
    if (this != &other) {
        if (owner_)
            owner_->leaveLimbo(origin_, name_);
        owner_ = std::exchange(other.owner_, nullptr);
        origin_ = other.origin_;
        name_ = std::move(other.name_);
    }
    return *this;
}

LimboSlot::~LimboSlot() {
    if (owner_)
        owner_->leaveLimbo(origin_, name_);
}

Antibot::Antibot(const config::AntibotConfig& config, Clock::time_point start) {
    auto settings = Settings::make(config);
    if (!settings)
        settings = Settings::make(config::AntibotConfig{});
    impl_ = std::make_unique<Impl>(std::move(*settings), start);
    impl_->feeds = std::make_unique<AddressFeeds>([this] { return this->config(); });
    impl_->keeper = std::make_unique<VerifiedKeeper>(*this);
}

bool Antibot::attackOn() const { return impl_->attackNow(); }

Antibot::~Antibot() {
    if (impl_) {
        impl_->keeper.reset();
        impl_->feeds.reset();
    }
}

std::expected<void, std::string> Antibot::apply(const config::AntibotConfig& config) {
    auto settings = Settings::make(config);
    if (!settings)
        return std::unexpected(settings.error());
    const std::lock_guard lock{impl_->settingsMutex};
    impl_->current = std::move(*settings);
    return {};
}

void Antibot::observe(Sink sink) {
    const std::lock_guard lock{impl_->settingsMutex};
    impl_->sink = std::move(sink);
}

std::shared_ptr<const Settings> Antibot::settings() const { return impl_->snapshot(); }

std::shared_ptr<const config::AntibotConfig> Antibot::config() const {
    auto settings = impl_->snapshot();
    const auto* config = &settings->config;
    return {std::move(settings), config};
}

bool Antibot::enabled() const { return impl_->snapshot()->config.enabled; }

Verdict Antibot::connection(const OriginKey& origin, Clock::time_point now) {
    const auto settings = impl_->snapshot();
    if (!settings->config.enabled)
        return {};
    impl_->connections.fetch_add(1, std::memory_order_relaxed);
    impl_->advance(*settings, now);
    impl_->perSecondConnections.fetch_add(1, std::memory_order_relaxed);
    impl_->surge(*settings, now);
    if (Settings::within(settings->trusted, origin))
        return {Action::kAllow, Reason::kTrusted, 0};
    if (Settings::within(settings->blocked, origin)) {
        impl_->denied.fetch_add(1, std::memory_order_relaxed);
        return {Action::kDeny, Reason::kBlocked, 0};
    }
    if ((!settings->geo.empty() && Settings::within(settings->geo, origin) &&
         (!settings->config.geoOnlyInAttack || impl_->attackNow())) ||
        impl_->feeds->imported(origin)) {
        impl_->denied.fetch_add(1, std::memory_order_relaxed);
        return {Action::kDeny, Reason::kBlocked, 0};
    }
    if (settings->config.antiVpnEnabled)
        (void)impl_->feeds->proxyKnown(origin, now);
    auto& shard = impl_->shardOf(origin);
    const std::lock_guard lock{shard.mutex};
    bool fresh = false;
    auto& record = impl_->recordOf(shard, origin, *settings, now, fresh);
    if (fresh) {
        impl_->perSecondFresh.fetch_add(1, std::memory_order_relaxed);
        record.add(settings->config.newOriginScore, now);
    }
    if (record.bannedUntil > now) {
        impl_->denied.fetch_add(1, std::memory_order_relaxed);
        return {Action::kDeny, Reason::kBanned, record.effective(now)};
    }
    return {Action::kAllow, Reason::kNone, record.effective(now)};
}

Verdict Antibot::handshake(const OriginKey& origin, std::string_view host, std::int32_t protocol,
                           Clock::time_point now) {
    const auto settings = impl_->snapshot();
    const auto& c = settings->config;
    if (!c.enabled || Settings::within(settings->trusted, origin))
        return {};
    if (std::ranges::find(c.blockedProtocols, protocol) != c.blockedProtocols.end()) {
        impl_->denied.fetch_add(1, std::memory_order_relaxed);
        return {Action::kDeny, Reason::kProtocol, 0};
    }
    const bool badShape = host.size() > static_cast<std::size_t>(c.maxHostLength) ||
                          std::ranges::any_of(host, [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
    if (badShape) {
        impl_->denied.fetch_add(1, std::memory_order_relaxed);
        return {Action::kDeny, Reason::kHost, 0};
    }
    if (c.exploitsEnabled && c.exploitScanNames && exploitText(host)) {
        exploitRefused(origin, now);
        impl_->denied.fetch_add(1, std::memory_order_relaxed);
        return {Action::kDeny, Reason::kExploit, 0};
    }
    if (settings->hosts.empty())
        return {};
    auto name = lowered(host);
    while (name.ends_with('.'))
        name.pop_back();
    if (settings->hosts.contains(name))
        return {};
    if (c.unknownHostScore == 0) {
        impl_->denied.fetch_add(1, std::memory_order_relaxed);
        return {Action::kDeny, Reason::kHost, 0};
    }
    auto& shard = impl_->shardOf(origin);
    const std::lock_guard lock{shard.mutex};
    bool fresh = false;
    auto& record = impl_->recordOf(shard, origin, *settings, now, fresh);
    record.add(c.unknownHostScore, now);
    return {Action::kAllow, Reason::kHost, record.effective(now)};
}

bool Antibot::statusAllowed(const OriginKey& origin, Clock::time_point now) {
    const auto settings = impl_->snapshot();
    const auto& c = settings->config;
    if (!c.enabled || Settings::within(settings->trusted, origin))
        return true;
    auto& shard = impl_->shardOf(origin);
    const std::lock_guard lock{shard.mutex};
    bool fresh = false;
    auto& record = impl_->recordOf(shard, origin, *settings, now, fresh);
    record.pingedAt = now;
    if (c.statusPerSecond == 0)
        return true;
    if (now - record.statusWindow >= std::chrono::seconds{1}) {
        record.statusWindow = now;
        record.statusCount = 0;
    }
    return ++record.statusCount <= c.statusPerSecond;
}

Verdict Antibot::login(const OriginKey& origin, std::string_view name, bool legacyClient, Clock::time_point now) {
    const auto settings = impl_->snapshot();
    const auto& c = settings->config;
    if (!c.enabled)
        return {};
    impl_->advance(*settings, now);
    const auto deny = [&](Reason reason, std::int32_t score) {
        impl_->denied.fetch_add(1, std::memory_order_relaxed);
        impl_->perSecondFailures.fetch_add(1, std::memory_order_relaxed);
        const Verdict verdict{Action::kDeny, reason, score};
        impl_->emit(Event::Kind::kVerdict, origin, name, verdict);
        return verdict;
    };
    if (Settings::within(settings->trusted, origin))
        return {Action::kAllow, Reason::kTrusted, 0};
    if (name.size() > config::kMaxMatchedNameBytes)
        return deny(Reason::kNamePattern, 0);
    if (std::ranges::any_of(settings->patterns,
                            [&](const std::regex& p) { return std::regex_match(name.begin(), name.end(), p); }))
        return deny(Reason::kNamePattern, 0);

    const bool verified = impl_->verifiedNow(*settings, name, origin, now);
    std::int32_t proxyScore = 0;
    if (c.antiVpnEnabled && !verified) {
        if (const auto proxy = impl_->feeds->proxyKnown(origin, now); proxy && *proxy) {
            if (c.antiVpnDeny)
                return deny(Reason::kProxy, 0);
            proxyScore = c.antiVpnScore;
        }
    }
    auto& shard = impl_->shardOf(origin);
    std::int32_t score = 0;
    bool banned = false;
    bool newBan = false;
    bool captchaFirst = false;
    bool pinged = false;
    {
        const std::lock_guard lock{shard.mutex};
        bool fresh = false;
        auto& record = impl_->recordOf(shard, origin, *settings, now, fresh);
        if (proxyScore > 0)
            record.add(proxyScore, now);
        const auto nameHash = hashOf(name, false), skeleton = hashOf(name, true);
        const auto known = std::find(record.names.begin(), record.names.begin() + record.nameCount, nameHash) !=
                           record.names.begin() + record.nameCount;
        if (!known && !verified) {
            if (c.maxNamesPerOrigin > 0 && record.nameCount >= c.maxNamesPerOrigin)
                record.add(c.namesPerOriginScore, now);
            if (std::find(record.skeletons.begin(), record.skeletons.begin() + record.nameCount, skeleton) !=
                record.skeletons.begin() + record.nameCount)
                record.add(c.similarNamesScore, now);
            const auto slot = std::min<std::size_t>(record.nameCount, kNameMemory - 1);
            record.names[slot] = nameHash;
            record.skeletons[slot] = skeleton;
            record.nameCount = static_cast<std::uint8_t>(std::min<std::size_t>(record.nameCount + 1, kNameMemory));
        }
        pinged = record.pingedAt != Clock::time_point{} &&
                 now - record.pingedAt <= std::chrono::seconds{c.pingValiditySeconds};
        if (!verified && !pinged && c.noPingScore > 0 && !impl_->attackNow())
            record.add(c.noPingScore, now);
        score = record.effective(now);
        if (record.bannedUntil <= now && score >= c.banScore && !verified) {
            if (c.captchaBeforeBan && c.captchaEnabled && c.limboMode != config::LimboMode::kOff && !record.crossed &&
                !impl_->attackNow()) {
                record.crossed = true;
                captchaFirst = true;
            } else {
                record.bannedUntil = now + std::chrono::seconds{c.banSeconds};
                banned = true;
                newBan = true;
            }
        }
        banned = banned || (record.bannedUntil > now && !verified);
    }
    if (newBan)
        impl_->emit(Event::Kind::kBanned, origin, name, Verdict{Action::kDeny, Reason::kBanned, score});
    if (banned)
        return deny(Reason::kBanned, score);
    if (verified)
        return {Action::kAllow, Reason::kVerified, score};
    const bool attack = impl_->attackNow();
    if (attack && c.requirePingInAttack && !pinged)
        return deny(Reason::kNoPing, score);
    const bool passthrough = impl_->passthroughOn(c, attack, now);
    if (passthrough && impl_->passthroughReturned(Impl::verifiedKey(name, origin), now, c, false)) {
        if (!impl_->takeEntry(c, now)) {
            impl_->denied.fetch_add(1, std::memory_order_relaxed);
            const Verdict busy{Action::kDeny, Reason::kBusy, score};
            impl_->emit(Event::Kind::kVerdict, origin, name, busy);
            return busy;
        }
        impl_->passthroughReturned(Impl::verifiedKey(name, origin), now, c, true);
        verify(name, origin, now);
        return {Action::kAllow, Reason::kVerified, score};
    }

    bool limbo = false;
    Reason why = Reason::kNone;
    switch (c.limboMode) {
    case config::LimboMode::kOff:
        break;
    case config::LimboMode::kAlways:
        limbo = true;
        why = Reason::kNone;
        break;
    case config::LimboMode::kAttack:
        limbo = attack;
        why = Reason::kAttack;
        break;
    case config::LimboMode::kSuspicious:
        limbo = attack || score >= c.limboScore;
        why = attack ? Reason::kAttack : Reason::kScore;
        break;
    }
    if (captchaFirst) {
        limbo = true;
        why = Reason::kScore;
    }
    if (!limbo && c.limboMode != config::LimboMode::kOff && c.captchaEnabled &&
        c.captchaMode == config::CaptchaMode::kSuspicious && score >= c.captchaScore) {
        limbo = true;
        why = Reason::kScore;
    }
    const auto chatWanted = [&] {
        return c.chatCheckEnabled && (c.chatCheckMode == config::CaptchaMode::kAlways ||
                                      (c.chatCheckMode == config::CaptchaMode::kAttack && attack) ||
                                      (c.chatCheckMode == config::CaptchaMode::kSuspicious && score >= c.captchaScore));
    };
    if (!limbo && c.limboMode != config::LimboMode::kOff && chatWanted()) {
        limbo = true;
        why = Reason::kScore;
    }
    if (!limbo && passthrough) {
        impl_->passthroughSend(Impl::verifiedKey(name, origin), now, c);
        return {Action::kDeny, Reason::kPassthrough, score};
    }
    if (!limbo)
        return {Action::kAllow, Reason::kNone, score};
    if (legacyClient) {
        if (c.legacy == config::LegacyAction::kKick)
            return deny(Reason::kLegacy, score);
        return {Action::kAllow, Reason::kLegacy, score};
    }
    const bool captcha =
        c.captchaEnabled &&
        (captchaFirst || impl_->hard.load(std::memory_order_relaxed) || c.captchaMode == config::CaptchaMode::kAlways ||
         (c.captchaMode == config::CaptchaMode::kAttack && attack) ||
         (c.captchaMode == config::CaptchaMode::kSuspicious && score >= c.captchaScore));
    const bool forcedCaptcha = captchaFirst || impl_->hard.load(std::memory_order_relaxed);
    const Action chosen = captcha && (forcedCaptcha || !chatWanted()) ? Action::kCaptcha
                          : chatWanted()                              ? Action::kChatCheck
                                                                      : Action::kLimbo;
    const Verdict verdict{chosen, why, score};
    impl_->emit(Event::Kind::kVerdict, origin, name, verdict);
    return verdict;
}

void Antibot::loginFailed(const OriginKey& origin, Clock::time_point now) {
    const auto settings = impl_->snapshot();
    if (!settings->config.enabled)
        return;
    impl_->perSecondFailures.fetch_add(1, std::memory_order_relaxed);
    auto& shard = impl_->shardOf(origin);
    const std::lock_guard lock{shard.mutex};
    bool fresh = false;
    auto& record = impl_->recordOf(shard, origin, *settings, now, fresh);
    record.add(settings->config.failedLoginScore, now);
}

std::optional<LimboSlot> Antibot::enterLimbo(const OriginKey& origin, std::string_view name, Clock::time_point now) {
    const auto settings = impl_->snapshot();
    const auto& c = settings->config;
    auto& shard = impl_->shardOf(origin);
    const std::lock_guard lock{shard.mutex};
    bool fresh = false;
    auto& record = impl_->recordOf(shard, origin, *settings, now, fresh);
    if (record.limbo >= c.maxLimboPerOrigin || impl_->limboNow.load(std::memory_order_relaxed) >= c.maxLimboSessions)
        return std::nullopt;
    if (c.onePerName) {
        const std::lock_guard names{impl_->limboNamesMutex};
        if (!impl_->limboNames.insert(std::string{name}).second)
            return std::nullopt;
    }
    ++record.limbo;
    impl_->limboNow.fetch_add(1, std::memory_order_relaxed);
    impl_->limboEntered.fetch_add(1, std::memory_order_relaxed);
    return LimboSlot{this, origin, std::string{name}};
}

void Antibot::leaveLimbo(const OriginKey& origin, const std::string& name) {
    {
        const std::lock_guard names{impl_->limboNamesMutex};
        impl_->limboNames.erase(name);
    }
    auto& shard = impl_->shardOf(origin);
    {
        const std::lock_guard lock{shard.mutex};
        if (const auto found = shard.records.find(origin); found != shard.records.end() && found->second.limbo > 0)
            --found->second.limbo;
    }
    impl_->limboNow.fetch_sub(1, std::memory_order_relaxed);
}

Antibot::LimboExit Antibot::limboPassed(const OriginKey& origin, std::string_view name, Clock::time_point now) {
    impl_->limboPassed.fetch_add(1, std::memory_order_relaxed);
    const auto settings = impl_->snapshot();
    const auto& c = settings->config;
    if (impl_->passthroughOn(c, impl_->attackNow(), now)) {
        impl_->passthroughSend(Impl::verifiedKey(name, origin), now, c);
        return LimboExit::kSendAway;
    }
    if (!Settings::within(settings->trusted, origin) && !impl_->verifiedNow(*settings, name, origin, now) &&
        !impl_->takeEntry(c, now))
        return LimboExit::kBusy;
    verify(name, origin, now);
    impl_->emit(Event::Kind::kVerified, origin, name, Verdict{Action::kAllow, Reason::kVerified, 0});
    return LimboExit::kEnter;
}

void Antibot::limboFailed(const OriginKey& origin, std::string_view name, Reason reason, Clock::time_point now) {
    impl_->limboFailed.fetch_add(1, std::memory_order_relaxed);
    impl_->perSecondFailures.fetch_add(1, std::memory_order_relaxed);
    const auto settings = impl_->snapshot();
    std::int32_t score = 0;
    bool newBan = false;
    {
        auto& shard = impl_->shardOf(origin);
        const std::lock_guard lock{shard.mutex};
        bool fresh = false;
        auto& record = impl_->recordOf(shard, origin, *settings, now, fresh);
        record.add(settings->config.limboFailureScore, now);
        score = record.effective(now);
        if (score >= settings->config.banScore) {
            newBan = record.bannedUntil <= now;
            record.bannedUntil = now + std::chrono::seconds{settings->config.banSeconds};
        }
    }
    impl_->emit(Event::Kind::kLimboFailed, origin, name, Verdict{Action::kDeny, reason, score});
    if (newBan)
        impl_->emit(Event::Kind::kBanned, origin, name, Verdict{Action::kDeny, Reason::kBanned, score});
}

void Antibot::verify(std::string_view name, const std::optional<OriginKey>& origin, Clock::time_point now) {
    const auto settings = impl_->snapshot();
    if (settings->config.verifiedTtlHours == 0)
        return;
    const std::lock_guard lock{impl_->verifiedMutex};
    if (impl_->verified.size() >= settings->config.verifiedMax) {
        std::erase_if(impl_->verified, [&](const auto& entry) { return entry.second.expiry <= now; });
        if (impl_->verified.size() >= settings->config.verifiedMax)
            impl_->verified.erase(impl_->verified.begin());
    }
    const auto expiry = now + std::chrono::hours{settings->config.verifiedTtlHours};
    impl_->verified[Impl::verifiedKey(name, origin)] =
        VerifiedEntry{expiry, lowered(name), origin ? storable(*origin) : std::string{}, false};
    if (origin) {
        if (impl_->knownOrigins.size() >= settings->config.verifiedMax)
            impl_->knownOrigins.clear();
        impl_->knownOrigins[hashOf(*origin)] = expiry;
    }
    if (impl_->store.save && settings->config.verifiedPersist)
        impl_->store.save(lowered(name), origin ? storable(*origin) : std::string{},
                          std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch() + (expiry - now))
                              .count());
}

void Antibot::transferIssued(const OriginKey& origin, Clock::time_point now) {
    const auto settings = impl_->snapshot();
    const std::lock_guard lock{impl_->passthroughMutex};
    if (impl_->transfers.size() >= settings->config.verifiedMax)
        impl_->transfers.clear();
    impl_->transfers[hashOf(origin)] = now;
}

bool Antibot::transferExpected(const OriginKey& origin, Clock::time_point now) {
    const auto window = std::chrono::seconds{impl_->snapshot()->config.passthroughWindowSeconds};
    const std::lock_guard lock{impl_->passthroughMutex};
    const auto found = impl_->transfers.find(hashOf(origin));
    return found != impl_->transfers.end() && now - found->second <= window;
}

bool Antibot::knownOrigin(const OriginKey& origin, Clock::time_point now) const {
    const std::lock_guard lock{impl_->verifiedMutex};
    const auto found = impl_->knownOrigins.find(hashOf(origin));
    return found != impl_->knownOrigins.end() && found->second > now;
}

bool Antibot::exploitText(std::string_view text) const {
    const auto settings = impl_->snapshot();
    if (!settings->config.exploitsEnabled || settings->exploits.empty())
        return false;
    return settings->exploits.matches(text.substr(0, 512));
}

void Antibot::penalize(const OriginKey& origin, std::int32_t points, Clock::time_point now) {
    if (points <= 0)
        return;
    const auto settings = impl_->snapshot();
    auto& shard = impl_->shardOf(origin);
    const std::lock_guard lock{shard.mutex};
    bool fresh = false;
    impl_->recordOf(shard, origin, *settings, now, fresh).add(points, now);
}

void Antibot::exploitRefused(const OriginKey& origin, Clock::time_point now) {
    impl_->exploitsRefused.fetch_add(1, std::memory_order_relaxed);
    const auto settings = impl_->snapshot();
    auto& shard = impl_->shardOf(origin);
    const std::lock_guard lock{shard.mutex};
    bool fresh = false;
    impl_->recordOf(shard, origin, *settings, now, fresh).add(settings->config.exploitScore, now);
}

void Antibot::persistVerified(VerifiedStore store) {
    {
        const std::lock_guard lock{impl_->verifiedMutex};
        impl_->store = store;
    }
    impl_->keeper->usePurge(std::move(store.purge));
}

std::vector<VerifiedInfo> Antibot::verifiedList(std::size_t limit) const {
    const auto now = Clock::now();
    const auto unixNow =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
    std::vector<VerifiedInfo> list;
    {
        const std::lock_guard lock{impl_->verifiedMutex};
        for (const auto& [key, entry] : impl_->verified)
            if (entry.expiry > now)
                list.push_back(
                    {entry.name, entry.origin,
                     unixNow.count() + std::chrono::duration_cast<std::chrono::seconds>(entry.expiry - now).count(),
                     entry.remote});
    }
    std::ranges::sort(list, [](const auto& a, const auto& b) { return a.expiresUnixSeconds > b.expiresUnixSeconds; });
    if (list.size() > limit)
        list.resize(limit);
    return list;
}

std::vector<OriginInfo> Antibot::originList(std::size_t limit, Clock::time_point now) {
    std::vector<OriginInfo> list;
    for (auto& shard : impl_->shards) {
        const std::lock_guard lock{shard.mutex};
        for (const auto& [key, record] : shard.records) {
            const auto score = record.effective(now);
            const bool banned = record.bannedUntil > now;
            if (score <= 0 && !banned)
                continue;
            list.push_back(
                {printable(key), score,
                 banned ? std::chrono::duration_cast<std::chrono::seconds>(record.bannedUntil - now).count() : 0,
                 record.limbo});
        }
    }
    std::ranges::sort(list, [](const auto& a, const auto& b) {
        return a.banSecondsLeft != b.banSecondsLeft ? a.banSecondsLeft > b.banSecondsLeft : a.score > b.score;
    });
    if (list.size() > limit)
        list.resize(limit);
    return list;
}

void Antibot::restoreVerified(std::string_view name, std::string_view origin, std::int64_t expiresUnixSeconds,
                              bool remote) {
    const auto settings = impl_->snapshot();
    if (settings->config.verifiedTtlHours == 0)
        return;
    const auto remaining =
        expiresUnixSeconds -
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (remaining <= 0)
        return;
    std::optional<OriginKey> key;
    if (!origin.empty()) {
        if (!config::AddressRange::parseAddress(origin))
            return;
        key = originOf(origin);
    }
    const auto expiry = Clock::now() + std::chrono::seconds{remaining};
    const std::lock_guard lock{impl_->verifiedMutex};
    if (impl_->verified.size() >= settings->config.verifiedMax)
        return;
    auto& entry = impl_->verified[Impl::verifiedKey(name, key)];
    if (remote && !entry.name.empty() && !entry.remote && entry.expiry >= expiry)
        return;
    entry = VerifiedEntry{expiry, lowered(name), key ? storable(*key) : std::string{}, remote};
    if (key)
        impl_->knownOrigins[hashOf(*key)] = expiry;
}

void Antibot::unverify(std::string_view name) {
    const auto persist = impl_->snapshot()->config.verifiedPersist;
    const std::lock_guard lock{impl_->verifiedMutex};
    const auto lower = lowered(name);
    std::erase_if(impl_->verified, [&](const auto& entry) { return entry.second.name == lower; });
    if (impl_->store.erase && persist)
        impl_->store.erase(lower);
}

bool Antibot::isVerified(std::string_view name, const std::optional<OriginKey>& origin, Clock::time_point now) const {
    const auto settings = impl_->snapshot();
    return origin ? impl_->verifiedNow(*settings, name, *origin, now) : [&] {
        const std::lock_guard lock{impl_->verifiedMutex};
        const auto found = impl_->verified.find(Impl::verifiedKey(name, std::nullopt));
        return found != impl_->verified.end() && found->second.expiry > now;
    }();
}

void Antibot::forceAttack(int mode) { impl_->manual.store(mode, std::memory_order_relaxed); }

void Antibot::ban(const OriginKey& origin, Clock::time_point now) {
    const auto settings = impl_->snapshot();
    bool wasBanned = false;
    std::int32_t score = 0;
    {
        auto& shard = impl_->shardOf(origin);
        const std::lock_guard lock{shard.mutex};
        bool fresh = false;
        auto& record = impl_->recordOf(shard, origin, *settings, now, fresh);
        wasBanned = record.bannedUntil > now;
        record.bannedUntil = now + std::chrono::seconds{settings->config.banSeconds};
        score = record.effective(now);
    }
    if (!wasBanned)
        impl_->emit(Event::Kind::kBanned, origin, {}, Verdict{Action::kDeny, Reason::kBanned, score});
}

bool Antibot::unban(const OriginKey& origin) {
    auto& shard = impl_->shardOf(origin);
    const std::lock_guard lock{shard.mutex};
    const auto found = shard.records.find(origin);
    if (found == shard.records.end())
        return false;
    shard.records.erase(found);
    return true;
}

bool Antibot::attackActive(Clock::time_point now) {
    const auto settings = impl_->snapshot();
    impl_->advance(*settings, now);
    return impl_->attackNow();
}

Stats Antibot::stats(Clock::time_point now) {
    Stats stats;
    stats.attackActive = attackActive(now);
    stats.connections = impl_->connections.load();
    stats.denied = impl_->denied.load();
    stats.limboEntered = impl_->limboEntered.load();
    stats.limboPassed = impl_->limboPassed.load();
    stats.limboFailed = impl_->limboFailed.load();
    stats.attacks = impl_->attacks.load();
    stats.exploits = impl_->exploitsRefused.load();
    stats.limboNow = impl_->limboNow.load();
    for (auto& shard : impl_->shards) {
        const std::lock_guard lock{shard.mutex};
        stats.origins += shard.records.size();
    }
    const std::lock_guard lock{impl_->verifiedMutex};
    stats.verified = impl_->verified.size();
    return stats;
}

} // namespace basalt::proxy::runtime::antibot
