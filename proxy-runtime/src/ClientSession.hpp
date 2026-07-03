#pragma once

#include "AccessPolicy.hpp"
#include "BackendAttempts.hpp"
#include "CookieCache.hpp"
#include "IdRenumbering.hpp"
#include "IdentityRegistry.hpp"
#include "PendingCookies.hpp"
#include "PendingResponses.hpp"
#include "ResourcePacks.hpp"
#include "antibot/Antibot.hpp"
#include "antibot/Captcha.hpp"
#include "antibot/LimboAssets.hpp"
#include "antibot/LimboRun.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/auth/Cfb8Cipher.hpp>
#include <basalt/wire/auth/LoginAuthentication.hpp>
#include <basalt/wire/common/placeholder/Placeholders.hpp>
#include <basalt/wire/config/PlayerUi.hpp>
#include <basalt/wire/config/ServerListConfig.hpp>
#include <basalt/wire/io/BudgetMemoryResource.hpp>
#include <basalt/wire/io/ByteBuffer.hpp>
#include <basalt/wire/io/EventLoop.hpp>
#include <basalt/wire/io/ResourceBudget.hpp>
#include <basalt/wire/io/TcpSocket.hpp>
#include <basalt/wire/multiversion/java/JavaTranslators.hpp>
#include <basalt/wire/plugins/PluginPackets.hpp>
#include <basalt/wire/protocol/ProtocolState.hpp>
#include <basalt/wire/protocol/play/HudPackets.hpp>

#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime {
struct PlayerCommandSpec;
}

namespace basalt::proxy::config {
struct RuntimeConfig;
struct BackendConfig;
} // namespace basalt::proxy::config

namespace basalt::proxy::runtime::detail {

class BackendRoutes;
class BackendDial;
class BackendLogin;
struct NativeForwarding;

class ClientSession {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kBufferCapacity = 1024;
    static constexpr std::size_t kBufferBytes = kBufferCapacity * 3;
    static constexpr std::size_t kAuthenticationBytes = 128 * 1024;
    static constexpr std::size_t kResolutionBytes = 16 * 1024;
    using StatusSource = std::function<std::vector<std::uint8_t>(
        std::int32_t protocol, std::string_view host, std::string_view address, const DecisionResult* changes)>;
    using AuthenticationStart = std::function<std::optional<std::stop_source>(wire::auth::OnlineLoginRequest)>;
    using ResolutionStart = std::function<std::optional<std::stop_source>(BackendAttempt, Clock::time_point)>;
    ClientSession(
        wire::io::TcpConnection connection, wire::io::ResourceBudget::Reservation slot,
        wire::io::ResourceBudget::Reservation buffers, const config::RuntimeConfig& settings, StatusSource status,
        std::span<const std::uint8_t> unavailable, Clock::time_point acceptedAt, bool online, bool checkAddress,
        std::span<const std::uint8_t> publicKey, std::span<const std::uint8_t> authFailure,
        AuthenticationStart authenticate, const BackendRoutes& routes, ResolutionStart resolve,
        const std::vector<config::BackendConfig>& backends, wire::io::ResourceBudget globalBuffers,
        std::int32_t compressionThreshold, bool acceptTransfers, IdentityRegistry& identities,
        const NativeForwarding* nativeForwarding = nullptr, std::shared_ptr<const void> topology = nullptr,
        std::function<bool(std::string_view, std::string_view)> playerCommand = {},
        std::function<std::vector<PlayerCommandSpec>(std::string_view)> playerCommandSpecs = {},
        config::TranslationMode translation = config::TranslationMode::Auto,
        std::function<void(std::string_view, std::string_view)> playerChat = {},
        std::function<void(std::string_view, std::string_view)> playerKick = {},
        std::function<void(std::string_view, std::int32_t)> statusPing = {},
        std::function<std::optional<std::string>(std::string_view, std::string_view, std::string_view)> playerLogin =
            {},
        std::function<std::string(std::string_view)> kickText = {},
        std::function<std::optional<std::vector<std::string>>(std::string_view, std::string_view)> commandSuggestions =
            {},
        std::function<std::optional<std::string>()> loadingMessage = {}, ClientAccess access = {},
        std::function<bool(std::string_view, std::string_view, std::span<const std::uint8_t>, bool)> pluginMessage = {},
        std::function<bool(DecisionRequest)> decide = {},
        std::function<void(std::string_view, std::int32_t)> latency = {});

    struct UiSource {
        std::function<std::shared_ptr<const wire::config::UiConfigs>()> configs;
        const wire::common::placeholder::PlaceholderRegistry* placeholders = nullptr;
    };

    void useUi(UiSource source) { uiSource_ = std::move(source); }

    ~ClientSession();
    [[nodiscard]] bool
    authenticated(std::expected<wire::auth::AuthenticatedLogin, wire::auth::LoginAuthenticationFailure> result,
                  wire::io::ResourceBudget::Reservation storage);
    [[nodiscard]] bool resolved(BackendAttempt attempt, std::vector<wire::io::TcpAddress> addresses,
                                wire::io::ResourceBudget::Reservation storage);
    /// Owner only. Preparation preserves the frontend play state.
    [[nodiscard]] bool prepareBackend(std::size_t target);
    [[nodiscard]] bool inPlay() const;
    /// Relays the backend's command tree with the plugins' player commands added; the tree untouched if unreadable.
    [[nodiscard]] bool relayCommandTree(std::span<const std::uint8_t> original);
    /// Queues a system chat line; ignored unless the client is in play.
    void tell(std::string_view text);
    void tell(const wire::text::Component& component);
    /// Disconnects the player with a MiniMessage reason; ignored unless the client is in play.
    void kick(std::string_view reason);
    void kick(const wire::text::Component& component);
    /// Owner only. Applies a plugin decision; a stale token is ignored.
    [[nodiscard]] bool decided(std::uint64_t token, const DecisionResult& result, bool expired = false);

    [[nodiscard]] bool awaiting(std::uint64_t token) const { return decisionPending() && token == decisionToken_; }

    /// Sends a plugin message to the client, or to the player's backend; ignored unless the client is in play.
    void sendPluginMessage(std::string_view channel, std::span<const std::uint8_t> data, bool towardBackend);
    void registerBungeeChannels();
    [[nodiscard]] std::string_view activeBackendName() const;
    /// Owner only. Lets plugin listeners see, edit and cancel the packets this session relays.
    void attachPackets(std::shared_ptr<wire::plugins::PluginPacketBus> bus, std::uint64_t connection,
                       std::size_t maxBytes);
    /// Owner only. Sends a plugin's packet toward the client or the backend; false when the session is in another
    /// phase.
    [[nodiscard]] bool pluginPacket(bool serverbound, std::int32_t phase, std::int32_t id,
                                    std::span<const std::uint8_t> body);
    bool announced = false;
    std::string announcedBackend;
    [[nodiscard]] bool requestBackendChange(std::size_t target);
    [[nodiscard]] bool requestBackendChange(std::string_view backend);
    [[nodiscard]] std::string_view playerName() const;
    [[nodiscard]] std::string playerUuid() const;
    [[nodiscard]] const std::string& clientAddress() const;
    [[nodiscard]] std::uint16_t clientPort() const;

    [[nodiscard]] std::int32_t clientProtocol() const noexcept { return clientProtocol_; }

    [[nodiscard]] bool candidatePrepared() const;
    [[nodiscard]] bool beginPreparedChange();
    [[nodiscard]] bool commitLegacyChange();
    [[nodiscard]] bool backendReady(const wire::io::IoReady& event);
    [[nodiscard]] bool timedOut(Clock::time_point now);

    struct BackendWatch {
        std::int64_t handle = -1;
        wire::io::IoInterest interest{false, false};
    };

    [[nodiscard]] std::array<BackendWatch, 3> backendWatches() const;

    [[nodiscard]] bool frontendSuspended() const { return frontendPaused_ && output_.readableBytes() == 0; }

    [[nodiscard]] bool ready(const wire::io::IoReady& event);
    [[nodiscard]] wire::io::IoInterest interest() const;
    [[nodiscard]] Clock::time_point deadline() const;
    [[nodiscard]] Clock::time_point baseDeadline() const;

    [[nodiscard]] std::int64_t handle() const { return connection_.nativeHandle(); }

    /// 0 handshake or status, 1 login, 2 configuration, 3 play.
    [[nodiscard]] std::size_t phase() const noexcept {
        if (state_ == State::kPlay)
            return 3;
        if (state_ == State::kConfiguration)
            return 2;
        if (state_ == State::kLimboLogin || state_ == State::kLimboConfiguration || state_ == State::kLimboPlay)
            return 1;
        return state_ == State::Handshake || state_ == State::StatusRequest || state_ == State::Ping ||
                       state_ == State::kProxyHeader
                   ? 0
                   : 1;
    }

    [[nodiscard]] bool accounted() const;
    std::uint64_t timer = 0;
    std::array<std::int64_t, 3> watchedBackends{-1, -1, -1};

private:
    enum class State {
        kProxyHeader,
        Handshake,
        StatusRequest,
        Ping,
        LoginStart,
        Key,
        Authenticating,
        kBackendLogin,
        kCookieResponse,
        kLoginAck,
        kConfiguration,
        kPlay,
        kLimboLogin,
        kLimboConfiguration,
        kLimboPlay
    };
    [[nodiscard]] wire::protocol::ProtocolState protocolState() const;
    [[nodiscard]] bool translateInbound();
    [[nodiscard]] bool queueNative(std::span<const std::uint8_t> packet);
    [[nodiscard]] bool queueTranslated(std::span<const std::uint8_t> packet);
    std::unique_ptr<wire::multiversion::java::JavaChain> translation_;
    std::unique_ptr<IdRenumbering> ids_;
    std::unique_ptr<wire::multiversion::java::JavaChain> upward_;
    [[nodiscard]] bool upgrades(std::int32_t backendProtocol) const;
    [[nodiscard]] bool upgradeFrame(std::span<const std::uint8_t> frame);
    [[nodiscard]] bool downgradeFrame(BackendLogin& target, std::span<const std::uint8_t> frame);
    [[nodiscard]] bool toBackend(BackendLogin& target, std::span<const std::uint8_t> frame);
    [[nodiscard]] bool numbered(wire::protocol::ProtocolState state) const;
    /// True when this backend protocol is the client's own, so its packets travel untranslated.
    [[nodiscard]] bool rawProtocol(std::int32_t protocol) const;
    [[nodiscard]] bool rawNow() const;
    [[nodiscard]] bool queueLoginPacket(std::span<const std::uint8_t> packet);
    [[nodiscard]] bool keepAliveVarInt() const;
    bool legacyRaw_ = false;
    [[nodiscard]] bool acceptOrigin(std::string_view address, std::uint16_t port);
    [[nodiscard]] bool beginJoin();
    [[nodiscard]] bool kickClient(std::string_view key, std::string_view fallback);
    /// A verdict when the packet is not to travel on, which is what `packet()` then returns; empty to relay it.
    [[nodiscard]] std::optional<bool> limited(std::int32_t id);
    [[nodiscard]] std::optional<bool> screenChat(std::int32_t id);
    [[nodiscard]] bool continueJoin();
    [[nodiscard]] bool startLimbo();
    [[nodiscard]] bool limboPacket(std::int32_t id, std::span<const std::uint8_t> raw);
    [[nodiscard]] bool limboTimer(Clock::time_point now);
    [[nodiscard]] bool limboStep(const antibot::LimboRun::Step& step);
    [[nodiscard]] bool limboKeepAlive(Clock::time_point now);
    [[nodiscard]] bool limboPassed();
    [[nodiscard]] bool limboFailed(antibot::Reason reason);
    [[nodiscard]] bool limboSendCaptcha();
    [[nodiscard]] bool completeLimboHandover();
    [[nodiscard]] bool queueFramed(std::span<const std::uint8_t> framed);
    [[nodiscard]] bool limboSystemChat(std::string_view key);
    [[nodiscard]] wire::text::Component antibotText(std::string_view key) const;
    [[nodiscard]] bool refuseAntibot(std::string_view key);
    [[nodiscard]] antibot::OriginKey origin() const;

    [[nodiscard]] bool limboState() const {
        return state_ == State::kLimboLogin || state_ == State::kLimboConfiguration || state_ == State::kLimboPlay;
    }

    [[nodiscard]] std::optional<bool> screenExploits(std::int32_t id);
    config::TranslationMode translationMode_;
    [[nodiscard]] std::int32_t speaksTo(const config::BackendConfig& backend, std::int32_t client) const;
    [[nodiscard]] bool routeAllowed(std::int32_t client, const config::BackendConfig& backend) const;
    bool awaitingJoin_ = false;
    [[nodiscard]] bool acceptsBackend(const config::BackendConfig& backend) const;
    std::int32_t clientProtocol_ = 0;
    [[nodiscard]] bool answerKnownPacks();
    [[nodiscard]] bool legacyClient() const;
    [[nodiscard]] bool answerSuggestions(const wire::protocol::play::CommandSuggestionRequestPacket& request);
    [[nodiscard]] bool refuse(std::string_view text);
    [[nodiscard]] bool refuseMiniMessage(std::string_view text);
    [[nodiscard]] bool refuseWith(std::string_view key, std::string_view fallback);
    void trackBossBar(std::span<const std::uint8_t> body);
    void trackPlayerList(std::int32_t id, std::span<const std::uint8_t> body);
    [[nodiscard]] bool clearClientState();
    [[nodiscard]] bool clearBossBars();
    [[nodiscard]] bool synthesizeClientPacket(std::int32_t id);
    bool synthesizing_ = false;
    enum class ConnectionStage { kIdle, kResolving, kConnecting };

    class SessionEndpoint;
    enum class Tapped { kPass, kDrop, kChanged, kFailed };
    /// Runs the plugin listeners on the packet in `source`, positioned after its id; `whole` is the edited packet.
    [[nodiscard]] Tapped tap(wire::io::ByteBuffer& source, bool serverbound, std::int32_t id,
                             std::span<const std::uint8_t>& whole);
    [[nodiscard]] bool injectPacket(bool serverbound, std::int32_t id, std::span<const std::uint8_t> body,
                                    bool intercept);
    [[nodiscard]] std::optional<wire::plugins::PacketPhase> tapPhase() const;
    [[nodiscard]] bool tapEditable(bool serverbound, std::int32_t id) const;
    std::shared_ptr<wire::plugins::PluginPacketBus> packets_;
    std::uint64_t connectionId_ = 0;
    std::size_t tapMaxBytes_ = 0;
    int tapDepth_ = 0;
    wire::plugins::PluginPacket tapped_;
    std::shared_ptr<SessionEndpoint> endpoint_;

    enum class PluginMessageResult { kRelay, kConsumed, kInvalid };
    [[nodiscard]] PluginMessageResult pluginMessage(bool fromBackend);
    [[nodiscard]] bool decode();
    [[nodiscard]] bool packet();
    [[nodiscard]] bool preLoginPacket(std::int32_t id);
    enum class CookiePublication { kPublished, kUnavailable, kFatal };
    [[nodiscard]] CookiePublication publishCookieRequest(BackendLogin& source);
    [[nodiscard]] bool cookieResponse();
    [[nodiscard]] bool backendChallenge(bool ping = false);
    [[nodiscard]] bool challengeResponse(bool pong = false);
    [[nodiscard]] bool backendResourcePack(bool push);
    [[nodiscard]] bool resourcePackResponse();
    [[nodiscard]] bool rememberSettings(std::span<const std::uint8_t> packet);
    [[nodiscard]] bool queue(std::span<const std::uint8_t> bytes);
    /// Like queue, but a server list answer larger than one buffer is fed out in pieces, in order.
    [[nodiscard]] bool queueOrdered(std::span<const std::uint8_t> bytes);
    void refillFromTail();
    [[nodiscard]] bool queuePacket(std::span<const std::uint8_t> packet);
    [[nodiscard]] bool recoverBackend();
    [[nodiscard]] bool recoveryFailed();
    [[nodiscard]] bool route();
    [[nodiscard]] bool routeNext();
    [[nodiscard]] bool candidateReady(const wire::io::IoReady& event);
    [[nodiscard]] bool backendEvent(const wire::io::IoReady& event);
    [[nodiscard]] bool backendFrame(std::span<const std::uint8_t> bytes);
    [[nodiscard]] bool advancePreparedChange();
    void discardCandidate();
    [[nodiscard]] bool acknowledgePreparedChange();
    [[nodiscard]] bool dial();
    [[nodiscard]] bool afterLogin();
    [[nodiscard]] bool beginRecovery();

    [[nodiscard]] bool decisionPending() const { return deciding_ || switchDecision_ || kickDeciding_; }

    [[nodiscard]] bool queueStatus(const DecisionResult* changes);
    [[nodiscard]] bool relayHeld();
    [[nodiscard]] bool routeInitial();
    [[nodiscard]] bool ask(DecisionKind kind, std::string_view backend, std::string_view from,
                           std::string_view text = {});
    [[nodiscard]] BackendLogin* backend() const;
    [[nodiscard]] BackendLogin* backendFor(BackendAttempt attempt) const;
    [[nodiscard]] Clock::time_point backendDeadline() const;
    void cancelConnectionAttempt();
    void closeBackends();
    void backpressure();
    [[nodiscard]] bool bufferedPacket() const;
    std::shared_ptr<const void> topology_;
    std::function<bool(std::string_view, std::string_view)> playerCommand_;
    std::function<bool(std::string_view, std::string_view, std::span<const std::uint8_t>, bool)> pluginMessage_;
    std::function<bool(DecisionRequest)> decide_;
    std::function<void(std::string_view, std::int32_t)> latency_;
    std::int64_t keepAliveToken_ = -1;
    Clock::time_point keepAliveSent_;
    std::vector<std::uint8_t> held_;
    bool kickDeciding_ = false;
    std::uint64_t decisionToken_ = 0;
    Clock::time_point decisionDeadline_ = Clock::time_point::max();
    DecisionKind decisionKind_ = DecisionKind::kLogin;
    bool deciding_ = false;
    bool switchDecision_ = false;
    bool preConnectAsked_ = false;
    std::size_t decisionTarget_ = 0;
    std::function<void(std::string_view, std::string_view)> playerChat_;
    std::function<void(std::string_view, std::string_view)> playerKick_;
    std::function<void(std::string_view, std::int32_t)> statusPing_;
    std::function<std::optional<std::string>(std::string_view, std::string_view, std::string_view)> playerLogin_;
    std::function<std::string(std::string_view)> kickText_;
    std::function<std::optional<std::string>()> loadingMessage_;
    std::vector<std::string> backendCommands_;
    std::function<std::vector<PlayerCommandSpec>(std::string_view)> playerCommandSpecs_;
    std::function<std::optional<std::vector<std::string>>(std::string_view, std::string_view)> commandSuggestions_;
    wire::io::ResourceBudget::Reservation slot_;
    wire::io::ResourceBudget localBuffers_;
    wire::io::BudgetMemoryResource memory_;
    PendingCookies pendingCookies_;
    PendingResponses pendingKeepAlives_;
    std::vector<std::array<std::uint8_t, 16>> bossBars_;
    std::vector<wire::common::Uuid> listedPlayers_;
    PendingResponses pendingPings_;
    CookieCache cookieCache_;
    ResourcePacks resourcePacks_;
    IdentityRegistry& identities_;
    const NativeForwarding* nativeForwarding_;
    bool identityClaimed_ = false;
    bool allocationReady_ = false;
    wire::io::TcpConnection connection_;
    const config::RuntimeConfig& settings_;
    StatusSource status_;
    /// The part of an answer that did not fit output_ yet; charged to the session budget like every buffer.
    wire::io::ByteBuffer tail_;
    std::span<const std::uint8_t> unavailable_;
    bool online_;
    bool checkAddress_;
    bool acceptTransfers_;
    std::int32_t compressionThreshold_;
    std::optional<std::int32_t> compression_;
    std::span<const std::uint8_t> publicKey_;
    std::span<const std::uint8_t> authFailure_;
    AuthenticationStart authenticate_;
    const BackendRoutes& routes_;
    std::string virtualHost_;
    std::int32_t statusProtocol_ = 0;
    std::uint16_t virtualPort_ = 0;
    std::span<const std::size_t> targets_;
    std::array<std::size_t, 1> redirected_{};
    std::array<std::size_t, 1> requestedTarget_{};
    ResolutionStart resolve_;
    std::size_t nextTarget_ = 0;
    BackendAttempts attempts_;
    ConnectionStage connectionStage_ = ConnectionStage::kIdle;
    std::optional<std::stop_source> resolution_;
    std::optional<wire::io::ResourceBudget::Reservation> resolutionLocal_;
    std::optional<wire::io::ResourceBudget::Reservation> resolutionStorage_;
    std::unique_ptr<BackendDial> dial_;
    std::optional<BackendAttempt> dialAttempt_;
    const std::vector<config::BackendConfig>& backends_;
    std::unique_ptr<BackendLogin> activeBackend_;
    std::unique_ptr<BackendLogin> candidateBackend_;
    bool finishingConfiguration_ = false;
    bool startingConfiguration_ = false;
    /// The proxy-drawn tab list, boss bar and scoreboard; see ClientSessionUi.cpp.
    [[nodiscard]] bool uiBackendPacket(std::int32_t id);
    void uiTick(Clock::time_point now);
    void uiLeavePlay();
    UiSource uiSource_;
    std::unique_ptr<wire::config::PlayerUi> ui_;
    std::optional<wire::config::PingRequest> uiRequest_;
    wire::common::placeholder::PlaceholderContext uiContext_;
    wire::io::ByteBuffer uiFrame_;
    Clock::time_point uiDeadline_ = Clock::time_point::max();
    std::int64_t uiTick_ = 0;
    std::uint8_t uiYielded_ = 0;
    bool uiReady_ = false;
    bool uiSendFailed_ = false;
    bool switchingBackend_ = false;
    bool changeWhenPrepared_ = false;
    bool recovering_ = false;
    std::bitset<256> recoveryVisited_;
    Clock::time_point connectDeadline_;
    std::array<std::uint8_t, 4> challenge_{};
    std::string name_;
    std::optional<std::stop_source> authentication_;
    std::optional<wire::io::ResourceBudget::Reservation> profileStorage_;
    std::optional<wire::io::ResourceBudget::Reservation> profileLocal_;
    std::optional<wire::auth::SessionProfile> profile_;
    std::optional<wire::auth::Cfb8Cipher> encryptor_;
    std::optional<wire::auth::Cfb8Cipher> decryptor_;
    wire::io::ByteBuffer clientSettings_;
    wire::io::ByteBuffer idIn_;
    wire::io::ByteBuffer idOut_;
    wire::io::ByteBuffer input_;
    wire::io::ByteBuffer frame_;
    wire::io::ByteBuffer output_;
    State state_ = State::Handshake;
    bool closing_ = false;
    bool backendPaused_ = false;
    bool frontendPaused_ = false;
    Clock::time_point readDeadline_;
    Clock::time_point writeDeadline_ = Clock::time_point::max();
    Clock::time_point loginDeadline_;
    ClientAccess access_;
    std::string originAddress_;
    std::uint16_t originPort_ = 0;
    wire::protocol::PacketLimiter limiter_;
    wire::protocol::SpamThrottle chatSpam_;
    std::int64_t exploitSecond_ = -1;
    std::int32_t tabCompletes_ = 0;
    std::int32_t commandsSent_ = 0;
    wire::protocol::SpamThrottle commandSpam_;
    Clock::time_point joinRetryAt_ = Clock::time_point::max();
    antibot::Action pendingLimbo_ = antibot::Action::kAllow;
    std::unique_ptr<antibot::LimboRun> limboRun_;
    std::optional<antibot::LimboSlot> limboSlot_;
    std::shared_ptr<const antibot::LimboPackets> limboPackets_;
    std::unique_ptr<antibot::CaptchaImage> captcha_;
    std::unique_ptr<antibot::BlockCaptcha> blockCaptcha_;
    std::vector<std::uint8_t> limboBrand_;
    bool limboHandover_ = false;
    bool limboOwnsChain_ = false;
};

} // namespace basalt::proxy::runtime::detail
