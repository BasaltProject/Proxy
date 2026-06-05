#include "QueryServer.hpp"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <random>
#include <string_view>
#include <utility>

namespace basalt::proxy::runtime::detail {
namespace {

constexpr std::uint8_t kMagic0 = 0xFE;
constexpr std::uint8_t kMagic1 = 0xFD;
constexpr std::uint8_t kTypeStat = 0;
constexpr std::uint8_t kTypeHandshake = 9;
constexpr std::size_t kFullStatLength = 15;
constexpr std::int64_t kChallengeLifetimeMillis = 30'000;
constexpr int kPollMillis = 500;
constexpr std::size_t kMaxDatagram = 1460;
constexpr std::size_t kMaxChallenges = 4096;

void writeString(std::vector<std::uint8_t>& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
}

[[nodiscard]] std::uint64_t peerKey(const wire::io::UdpEndpoint& endpoint) {
    return (static_cast<std::uint64_t>(endpoint.address) << 16U) | endpoint.port;
}

[[nodiscard]] std::int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

QueryServer::QueryServer(wire::io::UdpSocket socket, std::function<Info()> info)
    : socket_(std::move(socket)), info_(std::move(info)) {}

QueryServer::~QueryServer() { thread_.request_stop(); }

std::unique_ptr<QueryServer> QueryServer::start(std::uint16_t port, std::function<Info()> info,
                                                const wire::common::Logger& logger) {
    std::expected<wire::io::UdpSocket, wire::io::SocketError> socket = wire::io::UdpSocket::bind(port);
    if (!socket.has_value()) {
        logger.warn([port] { return "Unable to initialise query system on UDP port " + std::to_string(port); });
        return nullptr;
    }
    std::unique_ptr<QueryServer> server{new QueryServer(std::move(*socket), std::move(info))};
    QueryServer* raw = server.get();
    raw->thread_ = std::jthread{[raw](const std::stop_token& stop) { raw->run(stop); }};
    logger.info([port] { return "Query running on UDP port " + std::to_string(port); });
    return server;
}

void QueryServer::run(const std::stop_token& stop) {
    std::array<std::uint8_t, kMaxDatagram> buffer{};
    lastPrune_ = nowMillis();
    while (!stop.stop_requested()) {
        wire::io::UdpEndpoint from;
        const std::expected<std::optional<std::size_t>, wire::io::SocketError> got =
            socket_.receiveFrom(buffer, from, kPollMillis);
        const std::int64_t now = nowMillis();
        if (now - lastPrune_ >= kChallengeLifetimeMillis) {
            lastPrune_ = now;
            std::erase_if(challenges_, [now](const auto& entry) {
                return entry.second.issuedMillis < now - kChallengeLifetimeMillis;
            });
        }
        if (!got.has_value() || !got->has_value())
            continue;
        const std::vector<std::uint8_t> reply = answer(std::span{buffer}.first(**got), peerKey(from), now);
        if (!reply.empty())
            static_cast<void>(socket_.sendTo(reply, from));
    }
    socket_.close();
}

std::vector<std::uint8_t> QueryServer::answer(std::span<const std::uint8_t> request, std::uint64_t peer,
                                              std::int64_t now) {
    if (request.size() < 3 || request[0] != kMagic0 || request[1] != kMagic1) {
        return {};
    }
    if (request[2] == kTypeHandshake) {
        if (request.size() < 7) {
            return {};
        }
        if (challenges_.size() >= kMaxChallenges && !challenges_.contains(peer)) {
            return {};
        }
        static thread_local std::mt19937 random{std::random_device{}()};
        Challenge challenge;
        challenge.issuedMillis = now;
        challenge.token = static_cast<std::int32_t>(random() & 0xFFFFFFU);
        std::copy_n(request.begin() + 3, 4, challenge.ident.begin());
        challenges_[peer] = challenge;
        std::vector<std::uint8_t> reply{kTypeHandshake};
        reply.insert(reply.end(), challenge.ident.begin(), challenge.ident.end());
        writeString(reply, std::to_string(challenge.token));
        return reply;
    }
    if (request[2] != kTypeStat) {
        return {};
    }
    const auto known = challenges_.find(peer);
    if (known == challenges_.end() || request.size() < 11) {
        return {};
    }
    const std::int32_t token = static_cast<std::int32_t>((static_cast<std::uint32_t>(request[7]) << 24U) |
                                                         (static_cast<std::uint32_t>(request[8]) << 16U) |
                                                         (static_cast<std::uint32_t>(request[9]) << 8U) | request[10]);
    if (token != known->second.token) {
        return {};
    }
    const Info info = info_();
    std::vector<std::uint8_t> reply{kTypeStat};
    reply.insert(reply.end(), known->second.ident.begin(), known->second.ident.end());
    const std::string players = std::to_string(info.players.size());
    const std::string maxPlayers = std::to_string(info.maxPlayers);
    const std::string hostPort = std::to_string(info.serverPort);
    if (request.size() == kFullStatLength) {
        writeString(reply, "splitnum");
        reply.push_back(128);
        reply.push_back(0);
        for (const auto& [key, value] :
             std::initializer_list<std::pair<std::string_view, std::string_view>>{{"hostname", info.serverName},
                                                                                  {"gametype", "SMP"},
                                                                                  {"game_id", "MINECRAFT"},
                                                                                  {"version", info.version},
                                                                                  {"plugins", ""},
                                                                                  {"map", info.worldName},
                                                                                  {"numplayers", players},
                                                                                  {"maxplayers", maxPlayers},
                                                                                  {"hostport", hostPort},
                                                                                  {"hostip", info.hostIp}}) {
            writeString(reply, key);
            writeString(reply, value);
        }
        reply.push_back(0);
        reply.push_back(1);
        writeString(reply, "player_");
        reply.push_back(0);
        for (const std::string& name : info.players) {
            writeString(reply, name);
        }
        reply.push_back(0);
        return reply;
    }
    writeString(reply, info.serverName);
    writeString(reply, "SMP");
    writeString(reply, info.worldName);
    writeString(reply, players);
    writeString(reply, maxPlayers);
    reply.push_back(static_cast<std::uint8_t>(info.serverPort & 0xFF));
    reply.push_back(static_cast<std::uint8_t>((info.serverPort >> 8) & 0xFF));
    writeString(reply, info.hostIp);
    return reply;
}

} // namespace basalt::proxy::runtime::detail
