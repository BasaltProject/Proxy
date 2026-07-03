#include "LimboRun.hpp"

#include "LimboAssets.hpp"

#include <basalt/wire/protocol/ProtocolBuffer.hpp>
#include <basalt/wire/protocol/VarInt.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <random>

namespace basalt::proxy::runtime::antibot {
namespace {
constexpr std::int32_t kMaxConfigurationPackets = 64;
constexpr std::size_t kTicks = 96;
constexpr std::int32_t kLowestPhase = -2;
constexpr std::int32_t kHighestPhase = 3;

const std::array<double, kTicks>& fallTable() {
    static const auto kTable = [] {
        std::array<double, kTicks> table{};
        double velocity = 0, fallen = 0;
        for (std::size_t i = 0; i < kTicks; ++i) {
            table[i] = fallen;
            fallen -= velocity;
            velocity = std::max((velocity - 0.08) * 0.98, -3.92);
        }
        return table;
    }();
    return kTable;
}

[[nodiscard]] std::uint64_t entropy() {
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    return engine();
}

[[nodiscard]] bool sameCode(std::string_view typed, std::string_view answer) {
    while (!typed.empty() && std::isspace(static_cast<unsigned char>(typed.front())))
        typed.remove_prefix(1);
    while (!typed.empty() && std::isspace(static_cast<unsigned char>(typed.back())))
        typed.remove_suffix(1);
    return typed.size() == answer.size() && std::ranges::equal(typed, answer, [](unsigned char a, unsigned char b) {
               return std::toupper(a) == std::toupper(b);
           });
}
} // namespace

LimboRun::LimboRun(const config::AntibotConfig& config, Clock::time_point now, std::string captchaAnswer)
    : config_(config), deadline_(now + std::chrono::seconds{config.limboTimeoutSeconds}),
      nextKeepAlive_(Clock::time_point::max()), keepAliveSentAt_(Clock::time_point::max()),
      spawnY_(kLimboFloorY + config.dropHeight), answer_(std::move(captchaAnswer)), solved_(answer_.empty()),
      attemptsLeft_(config.captchaAttempts), captchaDeadline_(Clock::time_point::max()) {
    canCaptcha_ = config.softFailToCaptcha && config.captchaEnabled &&
                  config.captchaType == config::CaptchaType::kBlocks && answer_.empty();
}

LimboRun::Step LimboRun::fail(Reason reason) { return {Outcome::kFailed, reason, Signal::kNone}; }

LimboRun::Step LimboRun::loginAcknowledged() {
    if (phase_ != Phase::kLoginAck)
        return fail(Reason::kLimboFailed);
    phase_ = Phase::kKnownPacks;
    return {};
}

LimboRun::Step LimboRun::configuration(std::int32_t id, wire::io::ByteBuffer& body) {
    if (++packets_ > kMaxConfigurationPackets)
        return fail(Reason::kLimboFailed);
    switch (id) {
    case 0:
        information_ = true;
        return {};
    case 2: {
        if (!config_.checkBrand)
            return {};
        const auto channel = wire::protocol::readString(body, 256);
        if (body.failed())
            return fail(Reason::kLimboFailed);
        if (channel != "minecraft:brand")
            return {};
        auto brand = wire::protocol::readString(body, 64);
        if (body.failed() || brandSeen_ || brand.empty() ||
            std::ranges::any_of(brand, [](unsigned char c) { return c < 0x20 || c > 0x7e; }))
            return fail(Reason::kLimboFailed);
        std::ranges::transform(brand, brand.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (auto blocked : config_.blockedBrands) {
            std::ranges::transform(blocked, blocked.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (blocked == brand)
                return fail(Reason::kLimboFailed);
        }
        brandSeen_ = true;
        return {};
    }
    case 1:
    case 4:
    case 5:
    case 6:
    case 8:
    case 9:
        return {};
    case 7: {
        if (phase_ != Phase::kKnownPacks || knownPacksSeen_)
            return fail(Reason::kLimboFailed);
        const auto count = wire::protocol::varInt::read(body);
        if (body.failed() || count < 0 || count > 64)
            return fail(Reason::kLimboFailed);
        for (std::int32_t i = 0; i < count * 3; ++i)
            (void)wire::protocol::readString(body, 256);
        if (body.failed())
            return fail(Reason::kLimboFailed);
        knownPacksSeen_ = true;
        phase_ = Phase::kFinish;
        return {Outcome::kContinue, Reason::kNone, Signal::kSendConfigRest};
    }
    case 3:
        if (phase_ != Phase::kFinish || (config_.requireClientInformation && !information_) ||
            (config_.checkBrand && !brandSeen_))
            return fail(Reason::kLimboFailed);
        phase_ = Phase::kPlay;
        return {Outcome::kContinue, Reason::kNone, Signal::kSendPlayIntro};
    default:
        return fail(Reason::kLimboFailed);
    }
}

LimboRun::Step LimboRun::movement(double x, double y, double z, std::uint8_t flags, Clock::time_point now) {
    if (!teleported_)
        return {};
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return fail(Reason::kLimboFailed);
    if (degraded_)
        return {};
    if (config_.checkMovement) {
        const auto ticks = std::max(2.0, std::chrono::duration<double>(now - lastMoveAt_).count() * 20.0 + 2.0);
        const auto speed = std::hypot(x - lastX_, z - lastZ_);
        if (std::hypot(x - kLimboSpawnX, z - kLimboSpawnZ) > config_.maxDistance ||
            (moved_ && speed > config_.maxSpeed * ticks))
            return soften();
    }
    moved_ = true;
    lastX_ = x;
    lastZ_ = z;
    lastMoveAt_ = now;
    if (!config_.checkGravity)
        return {};
    const bool onGround = (flags & 1) != 0;
    if (y < kLimboFloorY - 0.05 || y > spawnY_ + 0.05)
        return soften();
    if (landed_) {
        if (y > kLimboFloorY + 0.2)
            return soften();
        groundSeen_ = groundSeen_ || onGround;
    } else {
        const double fallen = spawnY_ - y;
        if (fallen > 1e-6)
            ++moves_;
        else if (++stationary_ > kHighestPhase)
            return soften();
        if (onGround && y > kLimboFloorY + 0.1)
            return soften();
        const auto& table = fallTable();
        bool matches = false;
        for (std::int32_t p = kLowestPhase; p <= kHighestPhase && !matches; ++p) {
            const auto index = std::clamp<std::int32_t>(moves_ + p, 0, static_cast<std::int32_t>(kTicks) - 1);
            matches =
                std::abs(fallen - table[static_cast<std::size_t>(index)]) <= config_.fallTolerance + 0.02 * fallen;
        }
        if (!matches)
            return soften();
        if (y <= kLimboFloorY + 1e-3 && onGround) {
            if (moves_ < config_.minFallTicks)
                return soften();
            landed_ = true;
            groundSeen_ = true;
            gravityDone_ = true;
        }
    }
    return finished();
}

LimboRun::Step LimboRun::soften() {
    if (!canCaptcha_ || degraded_ || !answer_.empty())
        return fail(Reason::kLimboFailed);
    degraded_ = true;
    gravityDone_ = true;
    return {Outcome::kContinue, Reason::kNone, Signal::kNeedCaptcha};
}

void LimboRun::armCaptcha(std::string answer) {
    answer_ = std::move(answer);
    solved_ = false;
}

LimboRun::Step LimboRun::finished() {
    const bool gravity = !config_.checkGravity || gravityDone_;
    if (phase_ != Phase::kPlay || !gravity || answered_ == 0 || !solved_)
        return {};
    phase_ = Phase::kReconfigure;
    return {Outcome::kContinue, Reason::kNone, Signal::kSendReconfigure};
}

LimboRun::Step LimboRun::play(std::int32_t id, wire::io::ByteBuffer& body, Clock::time_point now) {
    if (++packets_ > config_.maxPackets)
        return fail(Reason::kLimboFailed);
    if (phase_ == Phase::kReconfigure)
        return id == 0x10 ? Step{Outcome::kPassed, Reason::kNone, Signal::kNone} : Step{};
    if (phase_ != Phase::kPlay)
        return fail(Reason::kLimboFailed);
    switch (id) {
    case 0: {
        const auto teleport = wire::protocol::varInt::read(body);
        if (body.failed() || teleport != kLimboTeleportId)
            return fail(Reason::kLimboFailed);
        teleported_ = true;
        return finished();
    }
    case 0x1c: {
        const auto echoed = body.readLong();
        if (body.failed() || !waiting_ || echoed != outstanding_)
            return fail(Reason::kLimboFailed);
        waiting_ = false;
        ++answered_;
        if (config_.maxLatencyMs > 0) {
            const auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(now - keepAliveSentAt_).count();
            slowAnswers_ = rtt > config_.maxLatencyMs ? slowAnswers_ + 1 : 0;
            if (slowAnswers_ >= config_.latencyStrikes)
                return soften();
        }
        keepAliveSentAt_ = Clock::time_point::max();
        nextKeepAlive_ = now + std::chrono::milliseconds{config_.keepAliveIntervalMs};
        return finished();
    }
    case 0x1e: {
        const auto x = body.readDouble(), y = body.readDouble(), z = body.readDouble();
        const auto flags = body.readByte();
        return body.failed() ? fail(Reason::kLimboFailed) : movement(x, y, z, flags, now);
    }
    case 0x1f: {
        const auto x = body.readDouble(), y = body.readDouble(), z = body.readDouble();
        const auto yaw = body.readFloat(), pitch = body.readFloat();
        const auto flags = body.readByte();
        if (body.failed() || !std::isfinite(yaw) || !std::isfinite(pitch) || std::abs(pitch) > 90.0F)
            return fail(Reason::kLimboFailed);
        return movement(x, y, z, flags, now);
    }
    case 0x20: {
        const auto yaw = body.readFloat(), pitch = body.readFloat();
        (void)body.readByte();
        return body.failed() || !std::isfinite(yaw) || !std::isfinite(pitch) || std::abs(pitch) > 90.0F
                   ? fail(Reason::kLimboFailed)
                   : Step{};
    }
    case 0x09: {
        if (answer_.empty() || solved_)
            return {};
        const auto text = wire::protocol::readString(body, 256);
        if (body.failed())
            return fail(Reason::kLimboFailed);
        if (sameCode(text, answer_)) {
            solved_ = true;
            return finished();
        }
        if (--attemptsLeft_ <= 0)
            return fail(Reason::kCaptchaFailed);
        return {Outcome::kContinue, Reason::kNone, Signal::kWrongCode};
    }
    default:
        return {};
    }
}

std::optional<std::int64_t> LimboRun::keepAliveDue(Clock::time_point now) {
    if (phase_ != Phase::kPlay || waiting_)
        return std::nullopt;
    if (nextKeepAlive_ == Clock::time_point::max())
        nextKeepAlive_ = now;
    if (now < nextKeepAlive_)
        return std::nullopt;
    outstanding_ = static_cast<std::int64_t>(entropy() ^ ++counter_);
    waiting_ = true;
    keepAliveSentAt_ = now;
    return outstanding_;
}

void LimboRun::captchaShown(Clock::time_point now) {
    captchaDeadline_ = now + std::chrono::seconds{config_.captchaTimeoutSeconds};
}

LimboRun::Step LimboRun::tick(Clock::time_point now) {
    if (now >= deadline_ && captchaDeadline_ == Clock::time_point::max())
        return fail(Reason::kTimeout);
    if (!solved_ && captchaDeadline_ != Clock::time_point::max() && now >= captchaDeadline_)
        return fail(Reason::kCaptchaFailed);
    if (waiting_ && now - keepAliveSentAt_ >= std::chrono::milliseconds{config_.keepAliveTimeoutMs})
        return fail(Reason::kTimeout);
    return {};
}

Clock::time_point LimboRun::nextWake() const {
    auto wake = captchaDeadline_ == Clock::time_point::max() ? deadline_ : captchaDeadline_;
    if (waiting_)
        wake = std::min(wake, keepAliveSentAt_ + std::chrono::milliseconds{config_.keepAliveTimeoutMs});
    else if (phase_ == Phase::kPlay && nextKeepAlive_ != Clock::time_point::max())
        wake = std::min(wake, nextKeepAlive_);
    return wake;
}

} // namespace basalt::proxy::runtime::antibot
