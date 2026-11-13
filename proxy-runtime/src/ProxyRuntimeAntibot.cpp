#include "ProxyRuntimeImpl.hpp"

namespace basalt::proxy::runtime {
namespace {
constexpr std::size_t kListRows = 25;

std::string antibotListing(antibot::Antibot& engine, std::string_view part, antibot::Clock::time_point now) {
    std::string out;
    if (part.empty() || part == "bans" || part == "reputation") {
        const auto origins = engine.originList(1000, now);
        std::size_t banned = 0, shown = 0;
        std::string rows;
        for (const auto& origin : origins) {
            const bool isBan = origin.banSecondsLeft > 0;
            banned += isBan ? 1 : 0;
            if ((part == "bans" && !isBan) || (part == "reputation" && isBan) || shown >= kListRows)
                continue;
            ++shown;
            rows += "\n  " + origin.origin + "  score " + std::to_string(origin.score) +
                    (isBan ? "  banned for " + std::to_string(origin.banSecondsLeft) + " s" : std::string{}) +
                    (origin.limbo > 0 ? "  in limbo " + std::to_string(origin.limbo) : std::string{});
        }
        out += "Origins (" + std::to_string(origins.size()) + " scored or banned, " + std::to_string(banned) +
               " banned):" + (rows.empty() ? std::string{" none"} : rows);
    }
    if (part.empty() || part == "verified") {
        const auto verified = engine.verifiedList(kListRows);
        const auto unixNow =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        out += std::string{out.empty() ? "" : "\n"} + "Verified (" + std::to_string(engine.stats(now).verified) +
               " in all, newest " + std::to_string(verified.size()) + "):";
        if (verified.empty())
            out += " none";
        for (const auto& entry : verified)
            out += "\n  " + entry.name + "  from " + (entry.origin.empty() ? std::string{"anywhere"} : entry.origin) +
                   "  expires in " + std::to_string(entry.expiresUnixSeconds - unixNow) + " s" +
                   (entry.remote ? "  (synced)" : std::string{});
    }
    return out;
}
} // namespace

std::string ProxyRuntime::antibotCommand(std::span<const std::string_view> words) {
    auto& engine = impl_->access.antibot;
    const auto now = antibot::Clock::now();
    const std::string_view verb = words.empty() ? std::string_view{"status"} : words[0];
    const std::string_view argument = words.size() > 1 ? words[1] : std::string_view{};
    if (verb == "status" || verb == "stats") {
        const auto stats = engine.stats(now);
        return "Antibot: attack mode " + std::string{stats.attackActive ? "on" : "off"} +
               "; attacks: " + std::to_string(stats.attacks) + "; connections: " + std::to_string(stats.connections) +
               "; refused: " + std::to_string(stats.denied) +
               "; limbo now/entered/passed/failed: " + std::to_string(stats.limboNow) + "/" +
               std::to_string(stats.limboEntered) + "/" + std::to_string(stats.limboPassed) + "/" +
               std::to_string(stats.limboFailed) + "; exploit attempts: " + std::to_string(stats.exploits) +
               "; origins tracked: " + std::to_string(stats.origins) + "; verified: " + std::to_string(stats.verified) +
               ".";
    }
    if (verb == "list" &&
        (argument.empty() || argument == "bans" || argument == "reputation" || argument == "verified"))
        return antibotListing(engine, argument, now);
    if (verb == "attack" && (argument == "on" || argument == "off" || argument == "auto")) {
        engine.forceAttack(argument == "on" ? 1 : argument == "off" ? -1 : 0);
        return "Attack mode is now " + std::string{argument == "auto" ? "automatic" : argument} + ".";
    }
    if ((verb == "ban" || verb == "unban") && !argument.empty()) {
        if (!config::AddressRange::parseAddress(argument))
            return "Not an IP address.";
        const auto origin = antibot::originOf(argument);
        if (verb == "ban") {
            engine.ban(origin, now);
            return "Origin banned.";
        }
        return engine.unban(origin) ? "Origin cleared." : "That origin is not tracked.";
    }
    if ((verb == "verify" || verb == "unverify" || verb == "verified") && wire::auth::validLoginName(argument)) {
        if (verb == "verify")
            antibotVerify(argument);
        else if (verb == "unverify")
            antibotUnverify(argument);
        else
            return std::string{argument} + (antibotVerified(argument) ? " is verified." : " is not verified.");
        return "Done.";
    }
    return "Usage: antibot status | list [bans|reputation|verified] | attack on|off|auto | ban <ip> | unban <ip> | "
           "verify|unverify|verified <name>.";
}

void ProxyRuntime::antibotVerify(std::string_view player) {
    impl_->access.antibot.verify(player, std::nullopt, antibot::Clock::now());
}

void ProxyRuntime::antibotUnverify(std::string_view player) { impl_->access.antibot.unverify(player); }

bool ProxyRuntime::antibotVerified(std::string_view player) const {
    return impl_->access.antibot.isVerified(player, std::nullopt, antibot::Clock::now());
}

namespace {
bool bedrockName(std::string_view name) {
    return !name.empty() && name.size() <= 16 &&
           std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) != 0 || c == ' ' || c == '_'; });
}
} // namespace

int ProxyRuntime::bedrockPacketsPerSecond() const { return impl_->access.antibot.config()->bedrockPacketsPerSecond; }

int ProxyRuntime::statusCacheMs() const {
    const auto config = impl_->access.antibot.config();
    if (!config->enabled || config->statusCacheMs <= 0)
        return 0;
    return !config->statusCacheOnlyInAttack || antibotAttack() ? config->statusCacheMs : 0;
}

int ProxyRuntime::bedrockLoginSeconds() const { return impl_->access.antibot.config()->bedrockLoginSeconds; }

bool ProxyRuntime::bedrockOffline(std::string_view address, bool connecting) {
    auto& engine = impl_->access.antibot;
    const auto config = engine.config();
    if (!config->enabled || !config->bedrockEnabled)
        return true;
    const auto origin = antibot::originOf(address);
    const auto now = antibot::Clock::now();
    if (!connecting)
        return engine.statusAllowed(origin, now);
    if (config->bedrockConnectScore > 0)
        engine.penalize(origin, config->bedrockConnectScore, now);
    const auto verdict = engine.connection(origin, now);
    if (verdict.action == antibot::Action::kDeny)
        return false;
    if (config->bedrockHalfOpenPerSecond > 0 && engine.attackActive(now)) {
        const auto second = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        if (impl_->bedrockSecond.exchange(second, std::memory_order_relaxed) != second)
            impl_->bedrockOpened.store(0, std::memory_order_relaxed);
        if (impl_->bedrockOpened.fetch_add(1, std::memory_order_relaxed) >=
            static_cast<std::uint32_t>(config->bedrockHalfOpenPerSecond))
            return false;
    }
    if (verdict.score >= config->banScore) {
        engine.ban(origin, now);
        return false;
    }
    return true;
}

std::optional<std::string> ProxyRuntime::bedrockLogin(std::string_view address, std::string_view name,
                                                      std::string_view xuid, bool authenticated) {
    auto& engine = impl_->access.antibot;
    const auto config = engine.config();
    if (!config->enabled || !config->bedrockEnabled)
        return std::nullopt;
    const auto origin = antibot::originOf(address);
    const auto now = antibot::Clock::now();
    const bool shape =
        bedrockName(name) &&
        (!authenticated || (!xuid.empty() && xuid.size() <= 20 &&
                            std::ranges::all_of(xuid, [](unsigned char c) { return std::isdigit(c) != 0; })));
    const auto say = [this](std::string_view key, std::string_view fallback) {
        const std::string text = impl_->services.kickText ? impl_->services.kickText(key, {}) : std::string{};
        return text.empty() ? std::string{fallback} : wire::text::toPlainText(wire::text::miniMessage::parse(text));
    };
    if (!shape) {
        engine.loginFailed(origin, now);
        return say("antibot-bedrock-unverified", "Your login could not be verified.");
    }
    std::string javaLike{name};
    std::ranges::replace(javaLike, ' ', '_');
    const auto verdict = engine.login(origin, javaLike, false, now);
    if (verdict.action == antibot::Action::kDeny) {
        engine.loginFailed(origin, now);
        return say("antibot-bedrock-throttled", "Too many connection attempts, try again later.");
    }
    const bool strict =
        verdict.action != antibot::Action::kAllow || (config->bedrockXboxInAttack && engine.attackActive(now));
    if (strict && !authenticated)
        return say("antibot-bedrock-xbox", "Sign in to Xbox Live to play here.");
    return std::nullopt;
}

void ProxyRuntime::bedrockFailed(std::string_view address) {
    impl_->access.antibot.loginFailed(antibot::originOf(address), antibot::Clock::now());
}

bool ProxyRuntime::antibotAttack() const { return impl_->access.antibot.attackActive(antibot::Clock::now()); }
} // namespace basalt::proxy::runtime
