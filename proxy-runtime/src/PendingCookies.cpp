#include "PendingCookies.hpp"

#include <algorithm>
#include <cstring>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::size_t kHeaderBytes = 19;

std::uint64_t read64(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 0;
    for (const auto byte : bytes)
        value = (value << 8) | byte;
    return value;
}
} // namespace

bool PendingCookies::add(std::string_view key, std::uint64_t storeRevision, BackendAttempt backend) {
    if (key.empty() || key.size() > 256 || count_ >= 32 || backend.generation == 0 || backend.target > 255 ||
        !keys_.tryReserve(key.size() + kHeaderBytes))
        return false;
    keys_.writeShort(static_cast<std::int16_t>(key.size()));
    keys_.writeLong(static_cast<std::int64_t>(storeRevision));
    keys_.writeLong(static_cast<std::int64_t>(backend.generation));
    keys_.writeByte(static_cast<std::uint8_t>(backend.target));
    for (const auto byte : key)
        keys_.writeByte(static_cast<std::uint8_t>(byte));
    ++count_;
    return true;
}

std::optional<PendingCookie> PendingCookies::consume(std::string_view key) {
    const auto bytes = keys_.readable();
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto length = (static_cast<std::size_t>(bytes[offset]) << 8) | bytes[offset + 1];
        if (std::ranges::equal(bytes.subspan(offset + kHeaderBytes, length), key, [](std::uint8_t left, char right) {
                return left == static_cast<std::uint8_t>(right);
            })) {
            const PendingCookie result{read64(bytes.subspan(offset + 2, 8)),
                                       {read64(bytes.subspan(offset + 10, 8)), bytes[offset + 18]}};
            auto tail = keys_.mutableSpan(offset);
            std::memmove(tail.data(), tail.data() + length + kHeaderBytes, tail.size() - length - kHeaderBytes);
            keys_.trimTail(length + kHeaderBytes);
            --count_;
            return result;
        }
        offset += length + kHeaderBytes;
    }
    return std::nullopt;
}

} // namespace basalt::proxy::runtime::detail
