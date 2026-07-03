#pragma once

#include "Antibot.hpp"

#include <atomic>
#include <mutex>

namespace basalt::proxy::runtime::antibot {

/// Logs refusals outside attacks, one summary per attack, and posts the webhook; fed every antibot event.
class AttackReport {
public:
    explicit AttackReport(Antibot& antibot) : antibot_(antibot) {}

    void onEvent(const Event& event);

private:
    void post(std::string_view kind, const std::string& text);
    void kernelBlock(const Event& event);

    Antibot& antibot_;
    std::mutex mutex_;
    Stats atStart_;
    Clock::time_point startedAt_;
    std::atomic<int> posting_{0};
    std::atomic<int> blocking_{0};
};

} // namespace basalt::proxy::runtime::antibot
