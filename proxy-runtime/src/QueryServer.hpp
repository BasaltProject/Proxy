#pragma once

#include <basalt/wire/common/logging/Logger.hpp>
#include <basalt/wire/io/UdpSocket.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace basalt::proxy::runtime::detail {

/// parity: `QueryThreadGs4`, the GameSpy 4 status protocol server list sites and monitors speak.
class QueryServer {
public:
    struct Info {
        std::string serverName;
        std::string worldName;
        std::string version;
        std::string hostIp;
        std::int32_t serverPort = 0;
        std::int32_t maxPlayers = 0;
        std::vector<std::string> players;
    };

    /// Null when the port cannot be bound, after saying why.
    [[nodiscard]] static std::unique_ptr<QueryServer> start(std::uint16_t port, std::function<Info()> info,
                                                            const wire::common::Logger& logger);

    QueryServer(const QueryServer&) = delete;
    QueryServer& operator=(const QueryServer&) = delete;
    ~QueryServer();

    /// The reply to one datagram, empty when it is to be ignored.
    [[nodiscard]] std::vector<std::uint8_t> answer(std::span<const std::uint8_t> request, std::uint64_t peer,
                                                   std::int64_t nowMillis);

private:
    struct Challenge {
        std::int64_t issuedMillis = 0;
        std::int32_t token = 0;
        std::array<std::uint8_t, 4> ident{};
    };

    QueryServer(wire::io::UdpSocket socket, std::function<Info()> info);

    void run(const std::stop_token& stop);

    wire::io::UdpSocket socket_;
    std::function<Info()> info_;
    std::map<std::uint64_t, Challenge> challenges_;
    std::int64_t lastPrune_ = 0;
    std::jthread thread_;
};

} // namespace basalt::proxy::runtime::detail
