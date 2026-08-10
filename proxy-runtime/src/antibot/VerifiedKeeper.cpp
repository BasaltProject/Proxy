#include "VerifiedKeeper.hpp"

#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/common/logging/Logger.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace basalt::proxy::runtime::antibot {
namespace {
constexpr std::string_view kHeader = "basalt-verified 1";
constexpr std::string_view kExtension = ".verified";
constexpr std::uintmax_t kMaxFileBytes = 16U << 20;
constexpr std::int64_t kSkewSeconds = 60;

[[nodiscard]] std::string syncId(const config::AntibotConfig& config) {
    if (!config.verifiedSyncId.empty())
        return config.verifiedSyncId;
    std::string name(64, '\0');
#ifndef _WIN32
    if (gethostname(name.data(), name.size() - 1) != 0)
        return "proxy";
#endif
    name.resize(std::char_traits<char>::length(name.c_str()));
    std::ranges::replace_if(name, [](unsigned char c) { return std::isalnum(c) == 0 && c != '-' && c != '_'; }, '_');
    return name.empty() ? "proxy" : name;
}

[[nodiscard]] std::int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

VerifiedKeeper::VerifiedKeeper(Antibot& antibot)
    : antibot_(antibot), worker_([this](std::stop_token stop) { run(stop); }) {}

void VerifiedKeeper::usePurge(decltype(VerifiedStore::purge) purge) {
    const std::lock_guard lock{mutex_};
    purge_ = std::move(purge);
}

void VerifiedKeeper::poke() {
    poked_.store(true, std::memory_order_relaxed);
    wake_.notify_all();
}

VerifiedKeeper::~VerifiedKeeper() {
    worker_.request_stop();
    wake_.notify_all();
}

void VerifiedKeeper::run(std::stop_token stop) {
    auto nextPurge = Clock::now();
    auto nextSync = Clock::now();
    while (!stop.stop_requested()) {
        const auto config = antibot_.config();
        const auto now = Clock::now();
        if (now >= nextPurge) {
            decltype(purge_) purge;
            {
                const std::lock_guard lock{mutex_};
                purge = purge_;
            }
            if (purge && config->verifiedPersist)
                purge(unixNow(), config->verifiedPurgeBatch, config->verifiedMax);
            nextPurge = now + std::chrono::seconds{config->verifiedPurgeSeconds};
        }
        const bool attacking = antibot_.attackOn();
        if (attacking)
            static_cast<void>(antibot_.attackActive(now));
        if (!config->verifiedSyncDirectory.empty() && config->verifiedTtlHours != 0) {
            if (now >= nextSync) {
                const auto id = syncId(*config);
                publish(*config, id);
                collect(*config, id);
                nextSync = now + std::chrono::seconds{config->verifiedSyncSeconds};
            }
        } else {
            nextSync = now + std::chrono::seconds{config->verifiedSyncSeconds};
        }
        std::unique_lock lock{mutex_};
        auto wakeAt = config->verifiedSyncDirectory.empty() ? nextPurge : std::min(nextPurge, nextSync);
        if (attacking)
            wakeAt = std::min(wakeAt, now + std::chrono::seconds{1});
        wake_.wait_until(lock, stop, wakeAt, [this] { return poked_.exchange(false, std::memory_order_relaxed); });
    }
}

void VerifiedKeeper::publish(const config::AntibotConfig& config, const std::string& id) {
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path directory{config.verifiedSyncDirectory};
    fs::create_directories(directory, error);
    std::ostringstream text;
    text << kHeader << '\n';
    for (const auto& entry : antibot_.verifiedList(config.verifiedMax))
        if (!entry.remote)
            text << entry.name << '\t' << entry.origin << '\t' << entry.expiresUnixSeconds << '\n';
    const fs::path target = directory / (id + std::string{kExtension});
    const fs::path temporary = directory / ("." + id + ".tmp");
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        file << text.str();
        if (!file) {
            wire::common::Logger{"Antibot"}.warn([&] { return "Could not write " + temporary.string() + "."; });
            return;
        }
    }
    fs::rename(temporary, target, error);
    if (error)
        wire::common::Logger{"Antibot"}.warn([&] { return "Could not publish " + target.string() + "."; });
}

void VerifiedKeeper::collect(const config::AntibotConfig& config, const std::string& id) {
    namespace fs = std::filesystem;
    std::error_code error;
    const std::int64_t horizon = unixNow() + static_cast<std::int64_t>(config.verifiedTtlHours) * 3600 + kSkewSeconds;
    std::size_t files = 0;
    for (fs::directory_iterator it{config.verifiedSyncDirectory, error}, end; !error && it != end;
         it.increment(error)) {
        const auto& path = it->path();
        if (path.extension() != kExtension || path.stem() == id || ++files > 256)
            continue;
        if (!it->is_regular_file(error) || it->is_symlink(error) || it->file_size(error) > kMaxFileBytes)
            continue;
        const auto modified = fs::last_write_time(path, error);
        auto& known = seen_[path.filename().string()];
        if (error || known == modified)
            continue;
        known = modified;
        std::ifstream file{path, std::ios::binary};
        std::string line;
        if (!std::getline(file, line) || line != kHeader)
            continue;
        std::size_t lines = 0;
        while (std::getline(file, line) && ++lines <= config.verifiedMax) {
            const auto first = line.find('\t');
            const auto second = first == std::string::npos ? first : line.find('\t', first + 1);
            if (second == std::string::npos)
                continue;
            const std::string_view name{line.data(), first};
            const std::string_view origin{line.data() + first + 1, second - first - 1};
            std::int64_t expires = 0;
            const char* begin = line.data() + second + 1;
            const char* last = line.data() + line.size();
            if (std::from_chars(begin, last, expires).ec != std::errc{} || !wire::auth::validLoginName(name))
                continue;
            antibot_.restoreVerified(name, origin, std::min(expires, horizon), true);
        }
    }
}

} // namespace basalt::proxy::runtime::antibot
