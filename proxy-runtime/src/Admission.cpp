#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/Admission.hpp>
#include <basalt/wire/io/TcpSocket.hpp>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace basalt::proxy::runtime {
namespace {
using Clock = Admission::Clock;

class Bucket {
public:
    Bucket(std::uint32_t rate, std::uint32_t burst, Clock::time_point start)
        : rate_(rate), capacity_(static_cast<std::uint64_t>(burst) * kUnit), credit_(capacity_), updated_(start) {}

    bool take(Clock::time_point now) {
        refill(now);
        if (credit_ < kUnit)
            return false;
        credit_ -= kUnit;
        return true;
    }

    bool full(Clock::time_point now) {
        refill(now);
        return credit_ == capacity_;
    }

private:
    void refill(Clock::time_point now) {
        if (now <= updated_)
            return;
        const auto elapsed =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - updated_).count());
        const auto missing = capacity_ - credit_;
        // Clamp before multiplying: even a long idle period cannot overflow the credit counter.
        const auto fillTime = missing / rate_ + (missing % rate_ != 0 ? 1 : 0);
        credit_ = elapsed >= fillTime ? capacity_ : credit_ + elapsed * rate_;
        updated_ = now;
    }

    static constexpr std::uint64_t kUnit = 1000000000;
    std::uint32_t rate_;
    std::uint64_t capacity_;
    std::uint64_t credit_;
    Clock::time_point updated_;
};

std::string originKey(std::string_view origin) {
    if (origin.empty() || origin.size() > 64)
        return {};
    auto endpoint = wire::io::TcpAddress::literal(origin, 1);
    if (!endpoint)
        return {};
    auto normalized = endpoint->host();
    if (normalized.starts_with("::ffff:") && wire::io::parseIpv4(std::string_view{normalized}.substr(7)))
        normalized.erase(0, 7);
    return normalized;
}
} // namespace

struct Admission::Impl {
    struct Origin {
        Bucket bucket;
        Clock::time_point seen;
    };

    config::AdmissionConfig config;
    Bucket global;
    Clock::time_point latest;
    mutable std::mutex mutex;
    std::unordered_map<std::string, Origin> origins;
    decltype(origins)::iterator sweep;

    Impl(const config::AdmissionConfig& settings, Clock::time_point start)
        : config(settings), global(settings.globalRate, settings.globalBurst, start), latest(start) {
        if (settings.globalRate == 0 || settings.globalBurst == 0 || settings.originRate == 0 ||
            settings.originBurst == 0 || settings.maxOrigins == 0 || settings.maxOrigins > 1000000 ||
            settings.originExpiryMs == 0)
            throw std::invalid_argument("invalid admission configuration");
        origins.reserve(settings.maxOrigins);
        sweep = origins.end();
    }

    void expire(Clock::time_point now) {
        // Fixed work per attempt; reserving maxOrigins prevents insertion from invalidating sweep.
        for (std::size_t visited = 0, count = std::min<std::size_t>(16, origins.size()); visited < count; ++visited) {
            if (sweep == origins.end())
                sweep = origins.begin();
            auto current = sweep++;
            if (now - current->second.seen >= std::chrono::milliseconds{config.originExpiryMs} &&
                current->second.bucket.full(now))
                origins.erase(current);
        }
    }
};

Admission::Admission(const config::AdmissionConfig& config, Clock::time_point start)
    : impl_(std::make_unique<Impl>(config, start)) {}

Admission::~Admission() = default;

AdmissionResult Admission::attempt(std::string_view literalOrigin, Clock::time_point now) {
    auto key = originKey(literalOrigin);
    if (key.empty())
        return AdmissionResult::InvalidOrigin;
    std::lock_guard lock{impl_->mutex};
    now = std::max(now, impl_->latest);
    impl_->latest = now;
    impl_->expire(now);
    // All attempts spend global credit, including those denied by a per-origin limit.
    if (!impl_->global.take(now))
        return AdmissionResult::GlobalRate;
    auto found = impl_->origins.find(key);
    if (found == impl_->origins.end()) {
        if (impl_->origins.size() == impl_->config.maxOrigins)
            return AdmissionResult::OriginCapacity;
        found = impl_->origins
                    .emplace(std::move(key),
                             Impl::Origin{Bucket{impl_->config.originRate, impl_->config.originBurst, now}, now})
                    .first;
    }
    found->second.seen = now;
    return found->second.bucket.take(now) ? AdmissionResult::Accepted : AdmissionResult::OriginRate;
}

std::size_t Admission::trackedOrigins() const {
    std::lock_guard lock{impl_->mutex};
    return impl_->origins.size();
}

} // namespace basalt::proxy::runtime
