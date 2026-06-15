#include "RemoteJson.hpp"

#include <basalt/proxy/remote/RemoteApi.hpp>
#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/proxy/runtime/Inspection.hpp>
#include <basalt/proxy/runtime/ProxyRuntime.hpp>
#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/multiversion/ProtocolVersions.hpp>
#include <basalt/wire/remote/ApiToken.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <thread>

namespace basalt::proxy::remote {
namespace {

using namespace json;
using wire::remote::HttpReply;
using wire::remote::http::errorBody;
using wire::remote::http::Request;

constexpr std::chrono::milliseconds kSessionsTimeout{500};
constexpr std::string_view kApiPrefix = "/v1/";

[[nodiscard]] HttpReply ok(JsonObject body, int status = 200) { return {status, text(std::move(body))}; }

[[nodiscard]] HttpReply failure(int status, std::string_view message) { return {status, errorBody(message)}; }

[[nodiscard]] std::vector<std::string_view> segments(std::string_view path) {
    std::vector<std::string_view> parts;
    while (!path.empty()) {
        if (path.front() == '/') {
            path.remove_prefix(1);
            continue;
        }
        const std::size_t end = path.find('/');
        parts.push_back(path.substr(0, end));
        if (end == std::string_view::npos)
            break;
        path.remove_prefix(end);
    }
    return parts;
}

[[nodiscard]] JsonValue sessionJson(const runtime::SessionInfo& session) {
    JsonObject object;
    object.set("name", session.name.empty() ? null() : str(session.name));
    object.set("uuid", session.uuid.empty() ? null() : str(session.uuid));
    object.set("edition", str("java"));
    object.set("phase", str(session.phase));
    object.set("backend", session.backend.empty() ? null() : str(session.backend));
    object.set("protocol", num(static_cast<std::uint64_t>(session.protocol)));
    object.set("version",
               session.protocol == 0 ? null() : str(wire::multiversion::protocolVersions::describe(session.protocol)));
    return obj(std::move(object));
}

[[nodiscard]] JsonValue protocolJson(const runtime::ProtocolSupport& support) {
    JsonObject object;
    object.set("protocol", num(static_cast<std::uint64_t>(support.protocol)));
    object.set("name", str(support.name));
    object.set("verified", flag(support.verified));
    return obj(std::move(object));
}

} // namespace

struct RemoteApi::Impl {
    config::RemoteConfig settings;
    RemoteHooks hooks;
    wire::common::Logger logger{"Remote"};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::unique_ptr<wire::remote::HttpServer> server;

    [[nodiscard]] std::expected<std::vector<runtime::SessionInfo>, HttpReply> snapshot() const {
        auto sessions = hooks.runtime->sessions(kSessionsTimeout);
        if (!sessions)
            return std::unexpected(failure(503, sessions.error()));
        return std::move(*sessions);
    }

    [[nodiscard]] HttpReply status() const {
        const auto& runtime = *hooks.runtime;
        const auto uptime =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
        JsonObject object;
        object.set("version", str(runtime::buildInfo::kVersion));
        object.set("minecraft", str(runtime::buildInfo::kMinecraftVersion));
        object.set("protocol", num(static_cast<std::uint64_t>(runtime::buildInfo::kProtocolVersion)));
        object.set("uptime-seconds", num(static_cast<std::uint64_t>(uptime.count())));
        object.set("port", num(runtime.port()));
        object.set("sessions", num(runtime.sessionCount()));
        object.set("backends", num(runtime.backendNames().size()));
        object.set("bedrock-players", hooks.bedrockPlaying ? num(hooks.bedrockPlaying()) : null());
        object.set("plugins", hooks.loadedPlugins ? num(hooks.loadedPlugins()) : null());
        return ok(std::move(object));
    }

    [[nodiscard]] HttpReply counters() const {
        const auto& runtime = *hooks.runtime;
        const runtime::RelayStats stats = runtime::relayStats();
        JsonObject ended;
        constexpr std::array<std::string_view, 4> kPhases = {"handshake", "login", "configuration", "play"};
        for (std::size_t phase = 0; phase < kPhases.size(); ++phase)
            ended.set(kPhases[phase], num(stats.endedIn[phase]));
        JsonObject object;
        object.set("sessions", num(runtime.sessionCount()));
        object.set("buffered-bytes", num(runtime.bufferedBytes()));
        object.set("accepted-connections", num(runtime.acceptedConnections()));
        object.set("rejected-connections", num(runtime.rejectedConnections()));
        object.set("bytes-toward-clients", num(runtime.bytesTowardClients()));
        object.set("bytes-toward-backends", num(runtime.bytesTowardBackends()));
        object.set("logins", num(stats.logins));
        object.set("backend-changes", num(stats.backendChanges));
        object.set("sessions-ended-in", obj(std::move(ended)));
        return ok(std::move(object));
    }

    [[nodiscard]] HttpReply backends() const {
        const auto sessions = hooks.runtime->sessions(kSessionsTimeout);
        std::vector<JsonValue> entries;
        for (const runtime::BackendInfo& backend : hooks.runtime->backends()) {
            JsonObject object;
            object.set("name", str(backend.name));
            object.set("host", str(backend.host));
            object.set("port", num(backend.port));
            object.set("protocol", backend.protocol == 0 ? null() : num(static_cast<std::uint64_t>(backend.protocol)));
            object.set("follows-client", flag(backend.protocol == 0));
            object.set("automatic", flag(backend.automatic));
            object.set("forwarding", str(backend.forwarding));
            std::optional<std::uint64_t> players;
            if (sessions)
                players = static_cast<std::uint64_t>(std::ranges::count_if(*sessions, [&backend](const auto& session) {
                    return session.phase == "play" && session.backend == backend.name;
                }));
            object.set("players", players ? num(*players) : null());
            entries.push_back(obj(std::move(object)));
        }
        JsonObject object;
        object.set("backends", list(std::move(entries)));
        return ok(std::move(object));
    }

    [[nodiscard]] HttpReply versions() const {
        std::vector<JsonValue> clients;
        for (const auto& support : runtime::clientProtocols())
            clients.push_back(protocolJson(support));
        std::vector<JsonValue> backendProtocols;
        for (const auto& support : runtime::backendProtocols())
            backendProtocols.push_back(protocolJson(support));
        JsonObject native;
        native.set("minecraft", str(runtime::buildInfo::kMinecraftVersion));
        native.set("protocol", num(static_cast<std::uint64_t>(runtime::buildInfo::kProtocolVersion)));
        JsonObject object;
        object.set("native", obj(std::move(native)));
        object.set("clients", list(std::move(clients)));
        object.set("backends", list(std::move(backendProtocols)));
        return ok(std::move(object));
    }

    [[nodiscard]] HttpReply sessions(bool playersOnly) const {
        auto sessions = snapshot();
        if (!sessions)
            return sessions.error();
        std::vector<JsonValue> entries;
        for (const auto& session : *sessions)
            if (!playersOnly || !session.name.empty())
                entries.push_back(sessionJson(session));
        JsonObject object;
        object.set("count", num(entries.size()));
        object.set(playersOnly ? "players" : "sessions", list(std::move(entries)));
        return ok(std::move(object));
    }

    [[nodiscard]] HttpReply player(std::string_view name) const {
        auto sessions = snapshot();
        if (!sessions)
            return sessions.error();
        const auto found =
            std::ranges::find_if(*sessions, [name](const auto& session) { return session.name == name; });
        if (found == sessions->end())
            return failure(404, "no such player");
        JsonObject object;
        object.set("player", sessionJson(*found));
        return ok(std::move(object));
    }

    [[nodiscard]] HttpReply run(std::string_view peer, const std::string& line) const {
        if (!hooks.command)
            return failure(503, "commands are not available");
        const CommandReply reply = hooks.command(line);
        logger.info([&] { return "API " + std::string{peer} + " ran: " + line; });
        switch (reply.outcome) {
        case runtime::CommandOutcome::Unknown:
            return failure(503, "the command is not available");
        case runtime::CommandOutcome::Denied:
            return failure(403, "the API source is not allowed to run that command");
        case runtime::CommandOutcome::Usage:
            return failure(400, "the command was refused as malformed");
        case runtime::CommandOutcome::Executed:
            break;
        }
        std::string joined;
        for (const std::string& text : reply.lines)
            joined += (joined.empty() ? "" : "\n") + text;
        if (joined.starts_with("Reload rejected"))
            return failure(422, joined);
        JsonObject object;
        object.set("status", str("executed"));
        object.set("message", str(joined));
        return ok(std::move(object));
    }

    [[nodiscard]] static bool plainName(std::string_view name) {
        return !name.empty() && name.size() <= 16 &&
               std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.'; });
    }

    [[nodiscard]] HttpReply send(std::string_view name, const Request& request, std::string_view peer) const {
        if (!settings.actions.send)
            return failure(403, "sending players is turned off in remote.toml");
        bool wellFormed = false;
        const std::string backend = stringField(request.body, "backend", wellFormed);
        if (!wellFormed || backend.empty())
            return failure(400, "the body must be a JSON object with a \"backend\" string");
        if (!plainName(name))
            return failure(400, "invalid player name");
        const auto known = hooks.runtime->backends();
        if (std::ranges::none_of(known, [&](const auto& entry) { return entry.name == backend; }))
            return failure(404, "no such backend");
        if (!hooks.playerOnline || !hooks.playerOnline(name))
            return failure(404, "no such player (names are case-sensitive)");
        return run(peer, "send " + std::string{name} + " " + backend);
    }

    [[nodiscard]] HttpReply kick(std::string_view name, const Request& request, std::string_view peer) const {
        if (!settings.actions.kick)
            return failure(403, "kicking players is turned off in remote.toml");
        bool wellFormed = false;
        std::string reason = stringField(request.body, "reason", wellFormed);
        if (!wellFormed)
            return failure(400, "the body must be empty or a JSON object with a \"reason\" string");
        if (!plainName(name))
            return failure(400, "invalid player name");
        if (!hooks.playerOnline || !hooks.playerOnline(name))
            return failure(404, "no such player");
        std::erase_if(reason, [](char c) { return c == '\n' || c == '\r'; });
        return run(peer, "kick " + std::string{name} + (reason.empty() ? std::string{} : " " + reason));
    }

    [[nodiscard]] HttpReply reload(std::string_view peer) const {
        if (!settings.actions.reload)
            return failure(403, "reloading is turned off in remote.toml");
        return run(peer, "reload");
    }

    [[nodiscard]] HttpReply route(const Request& request, std::string_view peer) const {
        if (!request.path.starts_with(kApiPrefix))
            return failure(404, "unknown endpoint");
        const auto parts = segments(std::string_view{request.path}.substr(kApiPrefix.size() - 1));
        const bool get = request.method == "GET";
        const bool post = request.method == "POST";
        const auto wrongMethod = [] { return failure(405, "method not allowed"); };
        if (parts.size() == 1) {
            const std::string_view leaf = parts[0];
            if (leaf == "reload")
                return post ? reload(peer) : wrongMethod();
            if (leaf != "status" && leaf != "counters" && leaf != "backends" && leaf != "versions" &&
                leaf != "players" && leaf != "sessions")
                return failure(404, "unknown endpoint");
            if (!get)
                return wrongMethod();
            if (leaf == "status")
                return status();
            if (leaf == "counters")
                return counters();
            if (leaf == "backends")
                return backends();
            if (leaf == "versions")
                return versions();
            return sessions(leaf == "players");
        }
        if (parts.size() == 2 && parts[0] == "players")
            return get ? player(parts[1]) : wrongMethod();
        if (parts.size() == 3 && parts[0] == "players" && parts[2] == "send")
            return post ? send(parts[1], request, peer) : wrongMethod();
        if (parts.size() == 3 && parts[0] == "players" && parts[2] == "kick")
            return post ? kick(parts[1], request, peer) : wrongMethod();
        return failure(404, "unknown endpoint");
    }
};

RemoteApi::RemoteApi(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RemoteApi::~RemoteApi() = default;

HttpReply RemoteApi::handle(const Request& request, std::string_view peer) { return impl_->route(request, peer); }

std::uint16_t RemoteApi::port() const noexcept { return impl_->server ? impl_->server->port() : 0; }

std::expected<std::unique_ptr<RemoteApi>, std::string>
RemoteApi::start(const config::RemoteConfig& settings, RemoteHooks hooks, const std::string& tokenPath) {
    if (!settings.api.enabled)
        return std::unique_ptr<RemoteApi>{};
    if (hooks.runtime == nullptr)
        return std::unexpected("the remote API needs a runtime");
    auto impl = std::make_unique<Impl>();
    impl->settings = settings;
    impl->hooks = std::move(hooks);
    auto token = wire::remote::loadOrCreateToken(tokenPath);
    if (!token)
        return std::unexpected(token.error());
    if (token->created)
        impl->logger.info([&] { return "Generated the API token in " + tokenPath + "."; });
    wire::remote::HttpServerLimits limits;
    limits.maxConnections = settings.api.maxConnections;
    limits.maxConnectionsPerAddress = settings.api.maxConnectionsPerAddress;
    limits.maxBodyBytes = settings.api.maxBodyBytes;
    limits.idleSeconds = settings.api.idleSeconds;
    limits.maxFailures = settings.api.maxFailures;
    limits.lockoutSeconds = settings.api.lockoutSeconds;
    limits.requestsPerMinute = settings.api.requestsPerMinute;
    Impl* raw = impl.get();
    auto server = wire::remote::HttpServer::start(
        settings.api.bind, settings.api.port, std::move(token->token), limits,
        [raw](const Request& request, std::string_view peer) { return raw->route(request, peer); },
        wire::common::Logger{"Remote"});
    if (!server)
        return std::unexpected("remote API: " + server.error());
    impl->server = std::move(*server);
    return std::unique_ptr<RemoteApi>(new RemoteApi(std::move(impl)));
}

} // namespace basalt::proxy::remote
