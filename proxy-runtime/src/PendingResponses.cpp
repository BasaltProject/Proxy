#include "PendingResponses.hpp"

#include <bit>
#include <cstring>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::size_t kEntryBytes = 25;

std::uint64_t read64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (const auto byte : bytes.subspan(offset, 8))
        value = (value << 8) | byte;
    return value;
}
} // namespace

std::optional<std::int64_t> PendingResponses::publish(std::int64_t token, BackendAttempt backend) {
    if (backend.generation == 0 || backend.target > 255 || entries_.readableBytes() >= 32 * kEntryBytes ||
        sequence_ >= limit_ || !entries_.tryReserve(kEntryBytes))
        return std::nullopt;
    const auto published = static_cast<std::int64_t>(++sequence_);
    entries_.writeLong(published);
    entries_.writeLong(token);
    entries_.writeLong(std::bit_cast<std::int64_t>(backend.generation));
    entries_.writeByte(static_cast<std::uint8_t>(backend.target));
    return published;
}

std::optional<ResponseOrigin> PendingResponses::consume(std::int64_t publicToken) {
    if (publicToken <= 0)
        return std::nullopt;
    const auto bytes = entries_.readable();
    for (std::size_t offset = 0; offset < bytes.size(); offset += kEntryBytes) {
        if (read64(bytes, offset) != static_cast<std::uint64_t>(publicToken))
            continue;
        const ResponseOrigin origin{std::bit_cast<std::int64_t>(read64(bytes, offset + 8)),
                                    {read64(bytes, offset + 16), bytes[offset + 24]}};
        auto tail = entries_.mutableSpan(offset);
        std::memmove(tail.data(), tail.data() + kEntryBytes, tail.size() - kEntryBytes);
        entries_.trimTail(kEntryBytes);
        return origin;
    }
    return std::nullopt;
}

} // namespace basalt::proxy::runtime::detail
