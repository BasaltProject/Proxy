#include "ConfigReader.hpp"

#include <basalt/proxy/config/AddressRange.hpp>
#include <basalt/proxy/config/AntibotConfig.hpp>

#include <filesystem>
#include <fstream>
#include <regex>

namespace basalt::proxy::config {
namespace {
using detail::Reader;
using detail::TomlTable;

template <typename T>
void number(Reader& reader, const TomlTable* table, std::string_view key, T& destination, std::int64_t min,
            std::int64_t max, std::string_view prefix) {
    reader.integer(table, key, destination, min, max, prefix);
}

void decimal(Reader& reader, const TomlTable* table, std::string_view key, double& destination, double min, double max,
             std::string_view prefix) {
    const auto* value = table ? table->find(key) : nullptr;
    if (!value)
        return;
    double read = 0;
    if (const auto* real = value->get<double>())
        read = *real;
    else if (const auto* whole = value->get<std::int64_t>())
        read = static_cast<double>(*whole);
    else {
        reader.fail(std::string{prefix} + std::string{key}, "has the wrong type");
        return;
    }
    if (read < min || read > max)
        reader.fail(std::string{prefix} + std::string{key}, "is outside its range");
    else
        destination = read;
}

void ranges(Reader& reader, const std::vector<std::string>& entries, std::string_view field) {
    for (const auto& entry : entries)
        if (!AddressRange::parse(entry))
            reader.fail(field, "entry '" + entry + "' is not an IP address or CIDR range");
}
} // namespace

std::expected<AntibotConfig, std::string> readAntibot(const TomlTable& table, std::string_view directory) {
    AntibotConfig result;
    Reader reader{std::string{directory} + "/antibot.toml", {}};
    reader.keys(&table,
                {"general", "lists", "reputation", "handshake", "names", "status", "attack-mode", "limbo", "captcha",
                 "passthrough", "notifications", "tcp", "kernel-block", "exploits", "bedrock", "verified"},
                "");

    const auto* general = reader.child(table, "general");
    reader.keys(general, {"enabled"}, "general.");
    reader.scalar(general, "enabled", result.enabled, "general.");

    const auto* lists = reader.child(table, "lists");
    reader.keys(lists, {"trusted", "blocked", "blocked-files"}, "lists.");
    reader.stringList(lists, "trusted", result.trusted, "lists.");
    reader.stringList(lists, "blocked", result.blocked, "lists.");
    std::vector<std::string> blockedFiles;
    reader.stringList(lists, "blocked-files", blockedFiles, "lists.");
    for (const auto& name : blockedFiles) {
        const auto path = std::filesystem::path{name}.is_absolute() ? std::filesystem::path{name}
                                                                    : std::filesystem::path{directory} / name;
        std::ifstream file{path};
        if (!file) {
            reader.fail("lists.blocked-files", "cannot read " + path.string());
            continue;
        }
        std::string line;
        while (std::getline(file, line)) {
            const auto begin = line.find_first_not_of(" \t\r");
            if (begin == std::string::npos || line[begin] == '#')
                continue;
            const auto end = line.find_last_not_of(" \t\r");
            result.blocked.push_back(line.substr(begin, end - begin + 1));
        }
    }
    ranges(reader, result.trusted, "lists.trusted");
    ranges(reader, result.blocked, "lists.blocked");

    const auto* reputation = reader.child(table, "reputation");
    reader.keys(reputation,
                {"max-origins", "origin-expiry-seconds", "ban-score", "ban-seconds", "new-origin-score",
                 "failed-login-score", "limbo-failure-score"},
                "reputation.");
    number(reader, reputation, "max-origins", result.maxOrigins, 16, 4000000, "reputation.");
    number(reader, reputation, "origin-expiry-seconds", result.originExpirySeconds, 10, 604800, "reputation.");
    number(reader, reputation, "ban-score", result.banScore, 1, 1000000, "reputation.");
    number(reader, reputation, "ban-seconds", result.banSeconds, 1, 2592000, "reputation.");
    number(reader, reputation, "new-origin-score", result.newOriginScore, 0, 1000000, "reputation.");
    number(reader, reputation, "failed-login-score", result.failedLoginScore, 0, 1000000, "reputation.");
    number(reader, reputation, "limbo-failure-score", result.limboFailureScore, 0, 1000000, "reputation.");

    const auto* handshake = reader.child(table, "handshake");
    reader.keys(handshake, {"allowed-hosts", "blocked-protocols", "max-host-length", "unknown-host-score"},
                "handshake.");
    reader.stringList(handshake, "allowed-hosts", result.allowedHosts, "handshake.");
    std::vector<std::string> blockedProtocols;
    reader.stringList(handshake, "blocked-protocols", blockedProtocols, "handshake.");
    for (const auto& entry : blockedProtocols) {
        try {
            result.blockedProtocols.push_back(std::stoi(entry));
        } catch (const std::exception&) {
            reader.fail("handshake.blocked-protocols", "entry '" + entry + "' is not a protocol number");
        }
    }
    number(reader, handshake, "max-host-length", result.maxHostLength, 1, 255, "handshake.");
    number(reader, handshake, "unknown-host-score", result.unknownHostScore, 0, 1000000, "handshake.");

    const auto* names = reader.child(table, "names");
    reader.keys(names, {"blocked-patterns", "max-names-per-origin", "similar-names-score", "names-per-origin-score"},
                "names.");
    reader.stringList(names, "blocked-patterns", result.blockedNamePatterns, "names.");
    number(reader, names, "max-names-per-origin", result.maxNamesPerOrigin, 0, 1000, "names.");
    number(reader, names, "similar-names-score", result.similarNamesScore, 0, 1000000, "names.");
    number(reader, names, "names-per-origin-score", result.namesPerOriginScore, 0, 1000000, "names.");
    for (const auto& pattern : result.blockedNamePatterns) {
        try {
            static_cast<void>(std::regex{pattern});
        } catch (const std::regex_error&) {
            reader.fail("names.blocked-patterns", "entry '" + pattern + "' is not a valid regular expression");
        }
    }

    const auto* status = reader.child(table, "status");
    reader.keys(status, {"requests-per-second", "require-ping-in-attack", "no-ping-score", "ping-validity-seconds"},
                "status.");
    number(reader, status, "requests-per-second", result.statusPerSecond, 0, 1000000, "status.");
    reader.scalar(status, "require-ping-in-attack", result.requirePingInAttack, "status.");
    number(reader, status, "no-ping-score", result.noPingScore, 0, 1000000, "status.");
    number(reader, status, "ping-validity-seconds", result.pingValiditySeconds, 1, 604800, "status.");

    const auto* attack = reader.child(table, "attack-mode");
    reader.keys(attack,
                {"enabled", "connections-per-second", "hard-connections-per-second", "failed-logins-per-second",
                 "new-origins-per-second", "confirm-seconds", "cooldown-seconds"},
                "attack-mode.");
    reader.scalar(attack, "enabled", result.attackEnabled, "attack-mode.");
    number(reader, attack, "hard-connections-per-second", result.attackHardConnectionsPerSecond, 0, 100000000,
           "attack-mode.");
    number(reader, attack, "connections-per-second", result.attackConnectionsPerSecond, 1, 10000000, "attack-mode.");
    number(reader, attack, "failed-logins-per-second", result.attackFailedLoginsPerSecond, 1, 10000000, "attack-mode.");
    number(reader, attack, "new-origins-per-second", result.attackNewOriginsPerSecond, 1, 10000000, "attack-mode.");
    number(reader, attack, "confirm-seconds", result.attackConfirmSeconds, 1, 3600, "attack-mode.");
    number(reader, attack, "cooldown-seconds", result.attackCooldownSeconds, 1, 86400, "attack-mode.");

    const auto* limbo = reader.child(table, "limbo");
    reader.keys(limbo,
                {"mode",
                 "suspicious-score",
                 "legacy-clients",
                 "timeout-seconds",
                 "max-packets",
                 "check-brand",
                 "blocked-brands",
                 "one-per-name",
                 "soft-fail-to-captcha",
                 "max-latency-ms",
                 "latency-strikes",
                 "keep-alive-interval-ms",
                 "keep-alive-timeout-ms",
                 "check-gravity",
                 "drop-height",
                 "min-fall-ticks",
                 "fall-tolerance",
                 "check-movement",
                 "max-speed",
                 "max-distance",
                 "require-client-information",
                 "max-per-origin",
                 "max-sessions"},
                "limbo.");
    std::string mode = "attack";
    std::string legacy = "skip";
    reader.scalar(limbo, "mode", mode, "limbo.");
    reader.scalar(limbo, "legacy-clients", legacy, "limbo.");
    if (mode == "off")
        result.limboMode = LimboMode::kOff;
    else if (mode == "attack")
        result.limboMode = LimboMode::kAttack;
    else if (mode == "suspicious")
        result.limboMode = LimboMode::kSuspicious;
    else if (mode == "always")
        result.limboMode = LimboMode::kAlways;
    else
        reader.fail("limbo.mode", "must be off, attack, suspicious or always");
    if (legacy == "skip")
        result.legacy = LegacyAction::kSkip;
    else if (legacy == "kick")
        result.legacy = LegacyAction::kKick;
    else
        reader.fail("limbo.legacy-clients", "must be skip or kick");
    number(reader, limbo, "suspicious-score", result.limboScore, 1, 1000000, "limbo.");
    number(reader, limbo, "timeout-seconds", result.limboTimeoutSeconds, 2, 600, "limbo.");
    reader.scalar(limbo, "check-brand", result.checkBrand, "limbo.");
    reader.stringList(limbo, "blocked-brands", result.blockedBrands, "limbo.");
    reader.scalar(limbo, "one-per-name", result.onePerName, "limbo.");
    reader.scalar(limbo, "soft-fail-to-captcha", result.softFailToCaptcha, "limbo.");
    number(reader, limbo, "max-packets", result.maxPackets, 100, 100000, "limbo.");
    number(reader, limbo, "max-latency-ms", result.maxLatencyMs, 0, 60000, "limbo.");
    number(reader, limbo, "latency-strikes", result.latencyStrikes, 1, 100, "limbo.");
    number(reader, limbo, "keep-alive-interval-ms", result.keepAliveIntervalMs, 250, 60000, "limbo.");
    number(reader, limbo, "keep-alive-timeout-ms", result.keepAliveTimeoutMs, 500, 120000, "limbo.");
    reader.scalar(limbo, "check-gravity", result.checkGravity, "limbo.");
    number(reader, limbo, "drop-height", result.dropHeight, 2, 40, "limbo.");
    number(reader, limbo, "min-fall-ticks", result.minFallTicks, 1, 40, "limbo.");
    decimal(reader, limbo, "fall-tolerance", result.fallTolerance, 0.01, 8.0, "limbo.");
    reader.scalar(limbo, "check-movement", result.checkMovement, "limbo.");
    decimal(reader, limbo, "max-speed", result.maxSpeed, 0.1, 10.0, "limbo.");
    decimal(reader, limbo, "max-distance", result.maxDistance, 1.0, 20.0, "limbo.");
    reader.scalar(limbo, "require-client-information", result.requireClientInformation, "limbo.");
    number(reader, limbo, "max-per-origin", result.maxLimboPerOrigin, 1, 1000, "limbo.");
    number(reader, limbo, "max-sessions", result.maxLimboSessions, 1, 65536, "limbo.");
    if (result.keepAliveTimeoutMs <= result.keepAliveIntervalMs)
        reader.fail("limbo.keep-alive-timeout-ms", "must be longer than keep-alive-interval-ms");

    const auto* captcha = reader.child(table, "captcha");
    reader.keys(captcha, {"enabled", "type", "mode", "score", "length", "attempts", "timeout-seconds"}, "captcha.");
    reader.scalar(captcha, "enabled", result.captchaEnabled, "captcha.");
    std::string captchaMode = "suspicious";
    reader.scalar(captcha, "mode", captchaMode, "captcha.");
    std::string captchaType = "blocks";
    reader.scalar(captcha, "type", captchaType, "captcha.");
    if (captchaType == "blocks")
        result.captchaType = CaptchaType::kBlocks;
    else if (captchaType == "map")
        result.captchaType = CaptchaType::kMap;
    else
        reader.fail("captcha.type", "must be blocks or map");
    if (captchaMode == "attack")
        result.captchaMode = CaptchaMode::kAttack;
    else if (captchaMode == "suspicious")
        result.captchaMode = CaptchaMode::kSuspicious;
    else if (captchaMode == "always")
        result.captchaMode = CaptchaMode::kAlways;
    else
        reader.fail("captcha.mode", "must be attack, suspicious or always");
    number(reader, captcha, "score", result.captchaScore, 1, 1000000, "captcha.");
    number(reader, captcha, "length", result.captchaLength, 3, 8, "captcha.");
    number(reader, captcha, "attempts", result.captchaAttempts, 1, 20, "captcha.");
    number(reader, captcha, "timeout-seconds", result.captchaTimeoutSeconds, 5, 600, "captcha.");

    const auto* passthrough = reader.child(table, "passthrough");
    reader.keys(passthrough, {"enabled", "mode", "window-seconds"}, "passthrough.");
    reader.scalar(passthrough, "enabled", result.passthroughEnabled, "passthrough.");
    std::string passthroughMode = "attack";
    reader.scalar(passthrough, "mode", passthroughMode, "passthrough.");
    if (passthroughMode != "attack" && passthroughMode != "always")
        reader.fail("passthrough.mode", "must be attack or always");
    result.passthroughAlways = passthroughMode == "always";
    number(reader, passthrough, "window-seconds", result.passthroughWindowSeconds, 5, 86400, "passthrough.");

    const auto* notifications = reader.child(table, "notifications");
    reader.keys(notifications, {"quiet-logs-in-attack", "webhook-url"}, "notifications.");
    reader.scalar(notifications, "quiet-logs-in-attack", result.quietLogsInAttack, "notifications.");
    reader.scalar(notifications, "webhook-url", result.webhookUrl, "notifications.");
    if (!result.webhookUrl.empty() && !result.webhookUrl.starts_with("https://"))
        reader.fail("notifications.webhook-url", "must be empty or an https:// address");

    const auto* tcp = reader.child(table, "tcp");
    reader.keys(tcp, {"enabled", "min-mss", "max-retransmits", "max-rtt-ms", "score"}, "tcp.");
    reader.scalar(tcp, "enabled", result.tcpEnabled, "tcp.");
    number(reader, tcp, "min-mss", result.tcpMinMss, 0, 65535, "tcp.");
    number(reader, tcp, "max-retransmits", result.tcpMaxRetransmits, 0, 255, "tcp.");
    number(reader, tcp, "max-rtt-ms", result.tcpMaxRttMs, 0, 60000, "tcp.");
    number(reader, tcp, "score", result.tcpScore, 0, 1000000, "tcp.");

    const auto* kernel = reader.child(table, "kernel-block");
    reader.keys(kernel, {"enabled", "only-in-attack", "command"}, "kernel-block.");
    reader.scalar(kernel, "enabled", result.kernelBlockEnabled, "kernel-block.");
    reader.scalar(kernel, "only-in-attack", result.kernelBlockOnlyInAttack, "kernel-block.");
    reader.stringList(kernel, "command", result.kernelBlockCommand, "kernel-block.");
    if (result.kernelBlockEnabled && result.kernelBlockCommand.empty())
        reader.fail("kernel-block.command", "is required when the block is enabled");

    const auto* bedrock = reader.child(table, "bedrock");
    reader.keys(bedrock, {"enabled", "packets-per-second", "login-seconds", "xbox-in-attack"}, "bedrock.");
    reader.scalar(bedrock, "enabled", result.bedrockEnabled, "bedrock.");
    reader.scalar(bedrock, "xbox-in-attack", result.bedrockXboxInAttack, "bedrock.");
    number(reader, bedrock, "packets-per-second", result.bedrockPacketsPerSecond, 0, 1000000, "bedrock.");
    number(reader, bedrock, "login-seconds", result.bedrockLoginSeconds, 1, 600, "bedrock.");

    const auto* exploits = reader.child(table, "exploits");
    reader.keys(exploits,
                {"enabled", "patterns", "scan-chat", "scan-commands", "scan-names", "scan-plugin-payloads",
                 "max-plugin-payload-bytes", "blocked-channels", "max-nbt-depth", "max-nbt-keys", "max-nbt-bytes",
                 "max-tab-completes-per-second", "max-commands-per-second", "score"},
                "exploits.");
    reader.scalar(exploits, "enabled", result.exploitsEnabled, "exploits.");
    reader.stringList(exploits, "patterns", result.exploitPatterns, "exploits.");
    reader.scalar(exploits, "scan-chat", result.exploitScanChat, "exploits.");
    reader.scalar(exploits, "scan-commands", result.exploitScanCommands, "exploits.");
    reader.scalar(exploits, "scan-names", result.exploitScanNames, "exploits.");
    reader.scalar(exploits, "scan-plugin-payloads", result.exploitScanPayloads, "exploits.");
    number(reader, exploits, "max-plugin-payload-bytes", result.maxPluginPayloadBytes, 0, 1048576, "exploits.");
    reader.stringList(exploits, "blocked-channels", result.blockedChannels, "exploits.");
    number(reader, exploits, "max-nbt-depth", result.maxNbtDepth, 0, 512, "exploits.");
    number(reader, exploits, "max-nbt-keys", result.maxNbtKeys, 0, 1000000, "exploits.");
    number(reader, exploits, "max-nbt-bytes", result.maxNbtBytes, 0, 16777216, "exploits.");
    number(reader, exploits, "max-tab-completes-per-second", result.maxTabCompletesPerSecond, 0, 1000, "exploits.");
    number(reader, exploits, "max-commands-per-second", result.maxCommandsPerSecond, 0, 1000, "exploits.");
    number(reader, exploits, "score", result.exploitScore, 0, 1000000, "exploits.");
    for (const auto& pattern : result.exploitPatterns)
        if (pattern.find_first_not_of('*') == std::string::npos)
            reader.fail("exploits.patterns", "an entry needs at least one character besides *");

    const auto* verified = reader.child(table, "verified");
    reader.keys(verified, {"ttl-hours", "max-entries"}, "verified.");
    number(reader, verified, "ttl-hours", result.verifiedTtlHours, 0, 87600, "verified.");
    number(reader, verified, "max-entries", result.verifiedMax, 16, 10000000, "verified.");

    if (reader.error)
        return std::unexpected(*reader.error);
    return result;
}

} // namespace basalt::proxy::config
