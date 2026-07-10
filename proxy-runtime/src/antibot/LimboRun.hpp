#pragma once

#include "Antibot.hpp"

#include <basalt/proxy/config/AntibotConfig.hpp>
#include <basalt/wire/io/ByteBuffer.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace basalt::proxy::runtime::antibot {

/// What a client has to do in the limbo, and the checks on it; the owner sends the packets and reports back.
class LimboRun {
public:
    enum class Phase : std::uint8_t { kLoginAck, kKnownPacks, kFinish, kPlay, kReconfigure };
    enum class Outcome : std::uint8_t { kContinue, kPassed, kFailed };
    enum class Signal : std::uint8_t {
        kNone,
        kSendConfigRest,
        kSendPlayIntro,
        kSendReconfigure,
        kWrongCode,
        kNeedCaptcha
    };

    struct Step {
        Outcome outcome = Outcome::kContinue;
        Reason reason = Reason::kNone;
        Signal signal = Signal::kNone;
    };

    LimboRun(const config::AntibotConfig& config, Clock::time_point now, std::string captchaAnswer = {});

    [[nodiscard]] Phase phase() const noexcept { return phase_; }

    [[nodiscard]] bool captcha() const noexcept { return !answer_.empty(); }

    [[nodiscard]] Step loginAcknowledged();
    [[nodiscard]] Step configuration(std::int32_t id, wire::io::ByteBuffer& body);
    [[nodiscard]] Step play(std::int32_t id, wire::io::ByteBuffer& body, Clock::time_point now);
    /// A keep-alive to send now, if one is due.
    [[nodiscard]] std::optional<std::int64_t> keepAliveDue(Clock::time_point now);
    /// A play ping to send now, once; the client has to answer it with the same id.
    [[nodiscard]] std::optional<std::int32_t> pingDue(Clock::time_point now);

    void expectEcho(bool on) noexcept { echo_ = on; }

    [[nodiscard]] Step tick(Clock::time_point now);
    [[nodiscard]] Clock::time_point nextWake() const;

    [[nodiscard]] std::int32_t attemptsLeft() const noexcept { return attemptsLeft_; }

    /// Starts the clock for solving a captcha, once the picture is on the client's screen.
    void captchaShown(Clock::time_point now);
    void armCaptcha(std::string answer);

    [[nodiscard]] bool degraded() const noexcept { return degraded_; }

private:
    [[nodiscard]] Step fail(Reason reason);
    [[nodiscard]] Step soften();
    [[nodiscard]] Step movement(double x, double y, double z, std::uint8_t flags, Clock::time_point now);
    [[nodiscard]] Step finished();

    config::AntibotConfig config_;
    Phase phase_ = Phase::kLoginAck;
    Clock::time_point deadline_;
    Clock::time_point nextKeepAlive_;
    Clock::time_point keepAliveSentAt_;
    std::int64_t outstanding_ = 0;
    bool waiting_ = false;
    std::int32_t answered_ = 0;
    std::uint64_t counter_ = 0;
    std::int32_t packets_ = 0;
    bool information_ = false;
    bool knownPacksSeen_ = false;
    bool teleported_ = false;
    bool landed_ = false;
    bool groundSeen_ = false;
    bool gravityDone_ = false;
    bool brandSeen_ = false;
    bool echo_ = false;
    bool pingWaiting_ = false;
    bool ponged_ = false;
    std::int32_t pingId_ = 0;
    Clock::time_point pingSentAt_;
    bool canCaptcha_ = false;
    bool degraded_ = false;
    std::int32_t slowAnswers_ = 0;
    std::int32_t moves_ = 0;
    std::int32_t stationary_ = 0;
    bool moved_ = false;
    double lastX_ = 0;
    double lastZ_ = 0;
    Clock::time_point lastMoveAt_;
    double spawnY_ = 0;
    std::string answer_;
    bool solved_ = true;
    std::int32_t attemptsLeft_ = 0;
    Clock::time_point captchaDeadline_;
};

} // namespace basalt::proxy::runtime::antibot
