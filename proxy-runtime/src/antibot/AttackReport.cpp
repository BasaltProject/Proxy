#include "AttackReport.hpp"

#include <basalt/wire/auth/HttpsClient.hpp>
#include <basalt/wire/common/logging/Logger.hpp>

#include <thread>

#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace basalt::proxy::runtime::antibot {
namespace {
constexpr int kMaxWebhooksInFlight = 2;

std::string jsonQuoted(std::string_view text) {
    std::string out{"\""};
    for (const char c : text) {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20)
            out.push_back(c);
    }
    return out + "\"";
}
} // namespace

void AttackReport::kernelBlock(const Event& event) {
#ifndef _WIN32
    const auto config = antibot_.config();
    if (!config->kernelBlockEnabled || config->kernelBlockCommand.empty() ||
        (config->kernelBlockOnlyInAttack && !antibot_.attackActive(Clock::now())) || blocking_.fetch_add(1) >= 4) {
        blocking_.fetch_sub(1);
        return;
    }
    std::vector<std::string> words;
    for (std::string word : config->kernelBlockCommand) {
        for (const auto& [token, value] : {std::pair<std::string_view, std::string>{"{ip}", event.origin},
                                           {"{family}", event.origin.find(':') != std::string::npos ? "6" : "4"},
                                           {"{seconds}", std::to_string(config->banSeconds)}})
            for (auto at = word.find(token); at != std::string::npos; at = word.find(token, at + value.size()))
                word.replace(at, token.size(), value);
        words.push_back(std::move(word));
    }
    std::thread{[this, words = std::move(words)]() mutable {
        std::vector<char*> argv;
        for (auto& word : words)
            argv.push_back(word.data());
        argv.push_back(nullptr);
        pid_t child = 0;
        if (posix_spawnp(&child, argv[0], nullptr, nullptr, argv.data(), environ) == 0) {
            int status = 0;
            waitpid(child, &status, 0);
        }
        blocking_.fetch_sub(1);
    }}.detach();
#else
    static_cast<void>(event);
#endif
}

void AttackReport::post(std::string_view kind, const std::string& text) {
    const auto config = antibot_.config();
    if (config->webhookUrl.empty() || posting_.fetch_add(1) >= kMaxWebhooksInFlight) {
        posting_.fetch_sub(1);
        return;
    }
    std::thread{[this, url = config->webhookUrl, trust = config->webhookTrustPem,
                 body = "{\"event\":" + jsonQuoted(kind) + ",\"content\":" + jsonQuoted(text) + "}"] {
        const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(body.data()), body.size()};
        const auto sent = wire::auth::httpsPost(url, bytes, "Content-Type: application/json\r\n", {.trustPem = trust});
        if (!sent || !sent->ok())
            wire::common::Logger{"Antibot"}.warn([&sent] {
                return "The attack webhook could not be delivered: " +
                       (sent ? "status " + std::to_string(sent->status) : sent.error().message) + ".";
            });
        posting_.fetch_sub(1);
    }}.detach();
}

void AttackReport::onEvent(const Event& event) {
    const wire::common::Logger logger{"Antibot"};
    const auto now = Clock::now();
    if (event.kind == Event::Kind::kBanned) {
        kernelBlock(event);
        return;
    }
    if (event.kind == Event::Kind::kAttackStarted) {
        {
            const std::lock_guard lock{mutex_};
            atStart_ = antibot_.stats(now);
            startedAt_ = now;
        }
        const std::string text = "Attack mode on: unverified players are held back.";
        logger.warn([&text] { return text; });
        post("attack_started", text);
    } else if (event.kind == Event::Kind::kAttackEnded) {
        Stats begin;
        Clock::time_point started;
        {
            const std::lock_guard lock{mutex_};
            begin = atStart_;
            started = startedAt_;
        }
        const auto end = antibot_.stats(now);
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - started).count();
        const std::string text = "Attack mode off after " + std::to_string(seconds) +
                                 " s: " + std::to_string(end.connections - begin.connections) + " connections, " +
                                 std::to_string(end.denied - begin.denied) + " refused, " +
                                 std::to_string(end.limboPassed - begin.limboPassed) + " limbo passed, " +
                                 std::to_string(end.limboFailed - begin.limboFailed) + " limbo failed, " +
                                 std::to_string(end.exploits - begin.exploits) + " exploit attempts.";
        logger.warn([&text] { return text; });
        post("attack_ended", text);
    } else if (event.kind == Event::Kind::kVerdict && event.verdict.action == Action::kDeny &&
               !(antibot_.config()->quietLogsInAttack && antibot_.attackActive(now))) {
        logger.info([&event] {
            return "Refused " + event.origin + (event.player.empty() ? std::string{} : " (" + event.player + ")") +
                   ": " + std::string{reasonName(event.verdict.reason)};
        });
    }
}

} // namespace basalt::proxy::runtime::antibot
