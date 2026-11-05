#include "AntibotImpl.hpp"

namespace basalt::proxy::runtime::antibot {
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

} // namespace basalt::proxy::runtime::antibot
