#include "CookieCache.hpp"

#include <basalt/wire/common/ByteView.hpp>
#include <basalt/wire/protocol/cookie/CookiePackets.hpp>

#include <cstring>
#include <limits>

namespace basalt::proxy::runtime::detail {

std::optional<CookieCache::Entry> CookieCache::find(std::string_view key) const {
    const auto bytes = values_.readable();
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto keySize = (static_cast<std::size_t>(bytes[offset]) << 8) | bytes[offset + 1];
        const auto payloadSize = (static_cast<std::size_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
        const auto size = 13 + keySize + payloadSize;
        std::uint64_t revision = 0;
        for (std::size_t i = 0; i < 8; ++i)
            revision = (revision << 8) | bytes[offset + 5 + i];
        if (wire::common::asText(bytes.subspan(offset + 13, keySize)) == key)
            return Entry{offset, size, offset + 13 + keySize, payloadSize, bytes[offset + 4] != 0, revision};
        offset += size;
    }
    return std::nullopt;
}

CachedCookie CookieCache::lookup(std::string_view key) const {
    const auto entry = find(key);
    if (!entry)
        return {};
    if (!entry->present)
        return {true, std::nullopt, entry->storeRevision};
    return {true, values_.readable().subspan(entry->payloadOffset, entry->payloadSize), entry->storeRevision};
}

bool CookieCache::remember(std::string_view key, std::optional<std::span<const std::uint8_t>> payload) {
    return update(key, payload, false);
}

bool CookieCache::store(std::string_view key, std::span<const std::uint8_t> payload) {
    return update(key, payload, true);
}

bool CookieCache::update(std::string_view key, std::optional<std::span<const std::uint8_t>> payload, bool stored) {
    if (stored && storeRevision_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    if (key.empty() || key.size() > 256 || (payload && payload->size() > wire::protocol::cookie::kMaxPayloadBytes))
        return false;
    const auto old = find(key);
    if (!old && count_ == 32)
        return false;
    const auto size = 13 + key.size() + (payload ? payload->size() : 0);
    const auto retained = values_.writerIndex() - (old ? old->size : 0);
    if (retained + size > 64 * 1024)
        return false;
    const auto growth = retained + size > values_.writerIndex() ? retained + size - values_.writerIndex() : 0;
    if (!values_.tryReserve(growth))
        return false;
    const auto revision = stored ? ++storeRevision_ : (old ? old->storeRevision : 0);
    if (old) {
        auto tail = values_.mutableSpan(old->offset);
        std::memmove(tail.data(), tail.data() + old->size, tail.size() - old->size);
        values_.trimTail(old->size);
    } else {
        ++count_;
    }
    values_.writeShort(static_cast<std::int16_t>(key.size()));
    values_.writeShort(static_cast<std::int16_t>(payload ? payload->size() : 0));
    values_.writeByte(payload.has_value() ? 1 : 0);
    values_.writeLong(static_cast<std::int64_t>(revision));
    values_.writeBytes(wire::common::asBytes(key));
    if (payload)
        values_.writeBytes(*payload);
    return true;
}

} // namespace basalt::proxy::runtime::detail
