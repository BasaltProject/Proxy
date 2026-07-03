#include "TcpProbe.hpp"

#ifdef __linux__
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace basalt::proxy::runtime::antibot {

std::optional<TcpFacts> probeTcp([[maybe_unused]] std::int64_t handle) {
#ifdef __linux__
    tcp_info info{};
    socklen_t size = sizeof info;
    if (getsockopt(static_cast<int>(handle), IPPROTO_TCP, TCP_INFO, &info, &size) != 0)
        return std::nullopt;
    return TcpFacts{info.tcpi_rtt, info.tcpi_snd_mss, info.tcpi_total_retrans};
#else
    return std::nullopt;
#endif
}

} // namespace basalt::proxy::runtime::antibot
