#include "CommandService.hpp"

#include <basalt/wire/common/logging/Logger.hpp>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::size_t kMaxQueued = 256;
}

CommandService::CommandService(CommandDispatcher& dispatcher, const Messages& messages)
    : dispatcher_(&dispatcher), messages_(&messages), worker_([this](std::stop_token stop) { work(stop); }) {}

CommandService::~CommandService() {
    worker_.request_stop();
    wake_.notify_all();
}

void CommandService::run(const CommandSource& source, std::string_view line) {
    if (!source.console && dispatcher_->logCommands())
        wire::common::Logger{"Commands"}.info(
            [&] { return source.name + " issued proxy command: /" + std::string{line}; });
    const CommandResult result = dispatcher_->execute(source, line);
    switch (result.outcome) {
    case CommandOutcome::Executed:
        return;
    case CommandOutcome::Unknown:
        source.reply(messages_->command("unknown"));
        return;
    case CommandOutcome::Denied:
        source.reply(messages_->command("no-permission"));
        return;
    case CommandOutcome::Usage:
        for (const std::string& usage : result.usage)
            source.reply(messages_->command("usage", {{"usage", usage}}));
        return;
    }
}

bool CommandService::submit(CommandSource source, std::string line) {
    if (!(source.console ? dispatcher_->knows(line) : dispatcher_->available(source, line)))
        return false;
    {
        const std::lock_guard lock{mutex_};
        if (queue_.size() >= kMaxQueued) {
            source.reply(messages_->command("busy"));
            return true;
        }
        queue_.push_back({std::move(source), std::move(line)});
    }
    wake_.notify_one();
    return true;
}

void CommandService::work(std::stop_token stop) {
    while (true) {
        Job job;
        {
            std::unique_lock lock{mutex_};
            if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); }))
                return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        run(job.source, job.line);
    }
}

} // namespace basalt::proxy::runtime::detail
