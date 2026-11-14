#pragma once

#include <basalt/wire/protocol/status/StatusPackets.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace basalt::proxy::runtime::detail {

/// Framed status response for the last payload seen; an equal payload reuses the bytes instead of serialising again.
class StatusFrameCache {
public:
    [[nodiscard]] std::vector<std::uint8_t> frame(wire::protocol::status::ServerStatusPayload payload);

private:
    struct Entry {
        wire::protocol::status::ServerStatusPayload payload;
        std::vector<std::uint8_t> frame;
    };

    std::mutex mutex_;
    std::shared_ptr<const Entry> last_;
};

} // namespace basalt::proxy::runtime::detail
