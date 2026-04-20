#include "PendingCookies.hpp"

#include <cstring>

namespace basalt::proxy::runtime::detail {

bool PendingCookies::add(std::string_view key, std::uint64_t storeRevision) {
    if (key.empty() || key.size() > 256 || count_ >= 32 || !keys_.tryReserve(key.size() + 10))
        return false;
    keys_.writeShort(static_cast<std::int16_t>(key.size()));
    keys_.writeLong(static_cast<std::int64_t>(storeRevision));
    keys_.writeBytes({reinterpret_cast<const std::uint8_t*>(key.data()), key.size()});
    ++count_;
    return true;
}

std::optional<std::uint64_t> PendingCookies::consume(std::string_view key) {
    const auto bytes = keys_.readable();
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto length = (static_cast<std::size_t>(bytes[offset]) << 8) | bytes[offset + 1];
        if (std::string_view{reinterpret_cast<const char*>(bytes.data() + offset + 10), length} == key) {
            std::uint64_t revision = 0;
            for (std::size_t i = 0; i < 8; ++i)
                revision = (revision << 8) | bytes[offset + 2 + i];
            auto tail = keys_.mutableSpan(offset);
            std::memmove(tail.data(), tail.data() + length + 10, tail.size() - length - 10);
            keys_.trimTail(length + 10);
            --count_;
            return revision;
        }
        offset += length + 10;
    }
    return std::nullopt;
}

} // namespace basalt::proxy::runtime::detail
