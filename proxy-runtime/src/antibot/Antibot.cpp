#include "AntibotImpl.hpp"

namespace basalt::proxy::runtime::antibot {
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

} // namespace basalt::proxy::runtime::antibot
