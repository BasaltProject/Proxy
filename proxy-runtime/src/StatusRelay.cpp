#include "StatusRelay.hpp"

namespace basalt::proxy::runtime::detail {
namespace {
constexpr int kStaleFactor = 4;
}

StatusRelay::StatusRelay(config::StatusRelayConfig settings, Resolve resolve)
    : settings_(settings), resolve_(std::move(resolve)), worker_([this](std::stop_token stop) { work(stop); }) {}

std::optional<wire::protocol::status::ServerStatusPayload> StatusRelay::get(const std::string& server) {
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::milliseconds ttl{settings_.ttlMs};
    const std::lock_guard lock{mutex_};
    const auto found = entries_.find(server);
    const bool fresh = found != entries_.end() && now - found->second.fetchedAt < ttl;
    if (!fresh && pending_.insert(server).second) {
        queue_.push_back(server);
        wake_.notify_one();
    }
    if (found == entries_.end() || now - found->second.fetchedAt >= ttl * kStaleFactor)
        return std::nullopt;
    return found->second.payload;
}

void StatusRelay::work(std::stop_token stop) {
    std::unique_lock lock{mutex_};
    while (!stop.stop_requested()) {
        if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); }))
            return;
        const std::string server = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        const auto target = resolve_(server);
        auto answer = target ? statusPing(*target, std::chrono::milliseconds{settings_.timeoutMs}, stop) : std::nullopt;
        lock.lock();
        pending_.erase(server);
        if (answer)
            entries_.insert_or_assign(server, Entry{std::move(*answer), std::chrono::steady_clock::now()});
        else
            entries_.erase(server);
    }
}

} // namespace basalt::proxy::runtime::detail
