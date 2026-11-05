#pragma once

#include "AddressFeeds.hpp"
#include "Antibot.hpp"
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
namespace impl {
constexpr std::size_t kShards = 16;
constexpr std::int64_t kDecayMs = 6000;
constexpr std::size_t kNameMemory = 8;

[[nodiscard]] inline std::uint64_t mix(std::uint64_t value) noexcept {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

[[nodiscard]] inline std::uint64_t hashOf(std::string_view text, bool skeleton) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(raw)));
        if (skeleton && (std::isdigit(c) || c == '_'))
            continue;
        hash = (hash ^ c) * 0x100000001b3ULL;
    }
    return mix(hash);
}

[[nodiscard]] inline std::uint64_t hashOf(const OriginKey& key) noexcept {
    std::uint64_t a = 0, b = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        a = (a << 8) | key.bytes[i];
        b = (b << 8) | key.bytes[8 + i];
    }
    return mix(a ^ mix(b));
}

[[nodiscard]] inline std::string lowered(std::string_view text) {
    std::string result{text};
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

[[nodiscard]] inline bool isV4(const OriginKey& key) {
    const auto& b = key.bytes;
    return std::all_of(b.begin(), b.begin() + 10, [](auto v) { return v == 0; }) && b[10] == 0xff && b[11] == 0xff;
}

[[nodiscard]] inline std::string storable(const OriginKey& key) {
    char text[64];
    const auto& b = key.bytes;
    if (isV4(key))
        std::snprintf(text, sizeof text, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
    else
        std::snprintf(text, sizeof text, "%x:%x:%x:%x::", b[0] << 8 | b[1], b[2] << 8 | b[3], b[4] << 8 | b[5],
                      b[6] << 8 | b[7]);
    return text;
}

[[nodiscard]] inline std::string printable(const OriginKey& key) {
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
} // namespace impl

using namespace impl;

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

namespace impl {
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
} // namespace impl

using namespace impl;

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

} // namespace basalt::proxy::runtime::antibot
