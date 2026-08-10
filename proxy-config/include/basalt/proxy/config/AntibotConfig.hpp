#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::wire::common::toml {
class TomlTable;
}

namespace basalt::proxy::config {

enum class LimboMode { kOff, kAttack, kSuspicious, kAlways };
enum class LegacyAction { kSkip, kKick };
enum class CaptchaMode { kAttack, kSuspicious, kAlways };
enum class CaptchaType { kBlocks, kMap };

struct AntibotConfig {
    bool enabled = true;

    std::vector<std::string> trusted;
    std::vector<std::string> blocked;

    std::size_t maxOrigins = 65536;
    std::uint32_t originExpirySeconds = 3600;
    std::int32_t banScore = 100;
    std::uint32_t banSeconds = 600;

    std::vector<std::string> allowedHosts;
    std::vector<std::string> blockedUrls;
    std::int32_t listRefreshMinutes = 60;
    bool antiVpnEnabled = false;
    std::string antiVpnUrl;
    std::string antiVpnMatch;
    std::int32_t antiVpnScore = 60;
    bool antiVpnDeny = false;
    std::int32_t antiVpnQueriesPerMinute = 30;
    std::int32_t antiVpnCacheHours = 24;
    std::vector<std::string> geoRanges;
    bool geoOnlyInAttack = false;
    std::vector<std::int32_t> blockedProtocols;
    std::int32_t maxHostLength = 255;
    std::int32_t unknownHostScore = 10;

    std::vector<std::string> blockedNamePatterns;
    std::int32_t maxNamesPerOrigin = 3;
    std::int32_t similarNamesScore = 30;
    std::int32_t namesPerOriginScore = 40;

    std::int32_t statusPerSecond = 5;
    std::int32_t statusCacheMs = 1000;
    bool statusCacheOnlyInAttack = true;
    bool requirePingInAttack = false;
    std::int32_t noPingScore = 5;
    std::uint32_t pingValiditySeconds = 600;

    std::int32_t newOriginScore = 5;
    std::int32_t failedLoginScore = 20;
    std::int32_t limboFailureScore = 35;
    bool captchaBeforeBan = true;

    bool attackEnabled = true;
    std::int32_t attackConnectionsPerSecond = 40;
    std::int32_t attackFailedLoginsPerSecond = 8;
    std::int32_t attackNewOriginsPerSecond = 25;
    std::int32_t attackHardConnectionsPerSecond = 0;
    std::uint32_t attackConfirmSeconds = 2;
    std::uint32_t attackCooldownSeconds = 60;
    std::int32_t attackEntriesPerSecond = 30;
    std::uint32_t attackPassthroughAfterSeconds = 5;

    LimboMode limboMode = LimboMode::kAttack;
    std::int32_t limboScore = 40;
    LegacyAction legacy = LegacyAction::kSkip;
    std::uint32_t limboTimeoutSeconds = 20;
    std::int32_t maxFrameBytes = 2048;
    bool checkEcho = true;
    bool checkBrand = true;
    std::vector<std::string> blockedBrands;
    bool onePerName = true;
    bool softFailToCaptcha = true;
    std::int32_t maxPackets = 1200;
    std::int32_t maxLatencyMs = 1500;
    std::int32_t latencyStrikes = 3;
    std::uint32_t keepAliveIntervalMs = 2000;
    std::uint32_t keepAliveTimeoutMs = 8000;
    bool checkGravity = true;
    std::int32_t dropHeight = 6;
    std::int32_t minFallTicks = 3;
    double fallTolerance = 0.35;
    bool checkMovement = true;
    double maxSpeed = 0.7;
    double maxDistance = 16.0;
    bool requireClientInformation = true;
    std::int32_t maxLimboPerOrigin = 2;
    std::uint32_t maxLimboSessions = 2048;

    bool captchaEnabled = true;
    CaptchaType captchaType = CaptchaType::kBlocks;
    CaptchaMode captchaMode = CaptchaMode::kSuspicious;
    std::int32_t captchaScore = 90;
    std::int32_t captchaLength = 4;
    std::int32_t captchaAttempts = 3;
    std::uint32_t captchaTimeoutSeconds = 45;

    bool passthroughEnabled = false;
    bool passthroughAlways = false;
    std::uint32_t passthroughWindowSeconds = 120;
    bool passthroughTransfer = true;
    std::string passthroughTransferHost;
    std::int32_t passthroughTransferPort = 0;

    bool chatCheckEnabled = false;
    CaptchaMode chatCheckMode = CaptchaMode::kAttack;
    std::string chatCheckRegisterCommand = "register";
    std::string chatCheckLoginCommand = "login";
    std::int32_t chatCheckMinLength = 4;
    std::int32_t chatCheckAttempts = 3;
    std::uint32_t chatCheckTimeoutSeconds = 45;

    bool quietLogsInAttack = true;
    std::string webhookUrl;
    std::string webhookTrustPem;

    bool tcpEnabled = false;
    std::int32_t tcpMinMss = 536;
    std::int32_t tcpMaxRetransmits = 4;
    std::int32_t tcpMaxRttMs = 0;
    std::int32_t tcpScore = 50;

    bool kernelBlockEnabled = false;
    bool kernelBlockOnlyInAttack = true;
    std::vector<std::string> kernelBlockCommand;

    bool exploitsEnabled = true;
    std::vector<std::string> exploitPatterns{"${*:", "jndi:", "ldap:", "rmi:", "dns:"};
    bool exploitScanChat = true;
    bool exploitScanCommands = true;
    bool exploitScanNames = true;
    bool exploitScanPayloads = true;
    std::int32_t maxPluginPayloadBytes = 8192;
    std::vector<std::string> blockedChannels;
    bool bedrockEnabled = true;
    std::int32_t bedrockPacketsPerSecond = 1500;
    std::int32_t bedrockLoginSeconds = 15;
    bool bedrockXboxInAttack = true;
    std::int32_t bedrockConnectScore = 2;
    std::int32_t bedrockHalfOpenPerSecond = 200;
    std::int32_t maxNbtDepth = 24;
    std::int32_t maxNbtKeys = 512;
    std::int32_t maxNbtBytes = 32768;
    std::int32_t maxTabCompletesPerSecond = 8;
    std::int32_t maxCommandsPerSecond = 15;
    std::int32_t exploitScore = 100;

    std::uint32_t verifiedTtlHours = 24;
    std::size_t verifiedMax = 100000;
    bool verifiedPersist = true;
    std::uint32_t verifiedPurgeSeconds = 300;
    std::int32_t verifiedPurgeBatch = 500;
    std::string verifiedSyncDirectory;
    std::uint32_t verifiedSyncSeconds = 30;
    std::string verifiedSyncId;

    friend bool operator==(const AntibotConfig&, const AntibotConfig&) = default;
};

[[nodiscard]] std::expected<AntibotConfig, std::string> readAntibot(const wire::common::toml::TomlTable& table,
                                                                    std::string_view directory);

} // namespace basalt::proxy::config
