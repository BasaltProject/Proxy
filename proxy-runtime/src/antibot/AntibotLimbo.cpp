#include "AntibotImpl.hpp"

namespace basalt::proxy::runtime::antibot {
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

} // namespace basalt::proxy::runtime::antibot
