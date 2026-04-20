#include "PendingCookies.hpp"

#include <cstring>

namespace basalt::proxy::runtime::detail {

bool PendingCookies::add(std::string_view key) {
    if (key.empty() || key.size() > 256 || count_ >= 32 || !keys_.tryReserve(key.size() + 2))
        return false;
    keys_.writeShort(static_cast<std::int16_t>(key.size()));
    keys_.writeBytes({reinterpret_cast<const std::uint8_t*>(key.data()), key.size()});
    ++count_;
    return true;
}

bool PendingCookies::consume(std::string_view key) {
    const auto bytes = keys_.readable();
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto length = (static_cast<std::size_t>(bytes[offset]) << 8) | bytes[offset + 1];
        if (std::string_view{reinterpret_cast<const char*>(bytes.data() + offset + 2), length} == key) {
            auto tail = keys_.mutableSpan(offset);
            std::memmove(tail.data(), tail.data() + length + 2, tail.size() - length - 2);
            keys_.trimTail(length + 2);
            --count_;
            return true;
        }
        offset += length + 2;
    }
    return false;
}

} // namespace basalt::proxy::runtime::detail
