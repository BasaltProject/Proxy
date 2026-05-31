#pragma once

#include "Messages.hpp"

#include <basalt/proxy/runtime/CommandDispatcher.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace basalt::proxy::runtime::detail {

/// Runs command lines: the console's on its own thread, players' on a worker so session owners never wait on them.
class CommandService {
public:
    CommandService(CommandDispatcher& dispatcher, const Messages& messages);
    ~CommandService();
    CommandService(const CommandService&) = delete;
    CommandService& operator=(const CommandService&) = delete;

    void run(const CommandSource& source, std::string_view line);

    /// True when the line names a command, which is then queued; false leaves it for the backend.
    [[nodiscard]] bool submit(CommandSource source, std::string line);

private:
    void work(std::stop_token stop);

    struct Job {
        CommandSource source;
        std::string line;
    };

    CommandDispatcher* dispatcher_;
    const Messages* messages_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Job> queue_;
    std::jthread worker_;
};

} // namespace basalt::proxy::runtime::detail
