#include "AntibotImpl.hpp"

namespace basalt::proxy::runtime::antibot {
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
