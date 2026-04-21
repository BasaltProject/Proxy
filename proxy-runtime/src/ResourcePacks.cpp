#include "ResourcePacks.hpp"

#include <cstring>
#include <limits>

namespace basalt::proxy::runtime::detail {
namespace {
constexpr std::size_t kEntryBytes = 33;
constexpr std::uint64_t kPublicNamespace = 0x626173616c744000;

std::uint64_t read64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t result = 0;
    for (const auto byte : bytes.subspan(offset, 8))
        result = (result << 8) | byte;
    return result;
}

bool matches(std::span<const std::uint8_t> entry, wire::common::Uuid id, BackendAttempt backend) {
    return read64(entry, 0) == id.high() && read64(entry, 8) == id.low() && read64(entry, 24) == backend.generation &&
           entry[32] == backend.target;
}

wire::common::Uuid publicId(std::uint64_t sequence) { return {kPublicNamespace, sequence}; }
} // namespace

std::optional<wire::common::Uuid> ResourcePacks::publish(wire::common::Uuid id, BackendAttempt backend) {
    if (backend.generation == 0 || backend.target > 255)
        return std::nullopt;
    const auto bytes = entries_.readable();
    for (std::size_t offset = 0; offset < bytes.size(); offset += kEntryBytes)
        if (matches(bytes.subspan(offset, kEntryBytes), id, backend))
            return publicId(read64(bytes, offset + 16));
    if (bytes.size() >= 32 * kEntryBytes || sequence_ == std::numeric_limits<std::uint64_t>::max() ||
        !entries_.tryReserve(kEntryBytes))
        return std::nullopt;
    entries_.writeLong(static_cast<std::int64_t>(id.high()));
    entries_.writeLong(static_cast<std::int64_t>(id.low()));
    entries_.writeLong(static_cast<std::int64_t>(++sequence_));
    entries_.writeLong(static_cast<std::int64_t>(backend.generation));
    entries_.writeByte(static_cast<std::uint8_t>(backend.target));
    return publicId(sequence_);
}

std::optional<ResourcePackOrigin> ResourcePacks::origin(wire::common::Uuid id) const {
    if (id.high() != kPublicNamespace)
        return std::nullopt;
    const auto bytes = entries_.readable();
    for (std::size_t offset = 0; offset < bytes.size(); offset += kEntryBytes)
        if (read64(bytes, offset + 16) == id.low())
            return ResourcePackOrigin{{read64(bytes, offset), read64(bytes, offset + 8)},
                                      {read64(bytes, offset + 24), bytes[offset + 32]}};
    return std::nullopt;
}

std::optional<wire::common::Uuid> ResourcePacks::remove(wire::common::Uuid id, BackendAttempt backend) {
    const auto bytes = entries_.readable();
    for (std::size_t offset = 0; offset < bytes.size(); offset += kEntryBytes) {
        if (!matches(bytes.subspan(offset, kEntryBytes), id, backend))
            continue;
        const auto result = publicId(read64(bytes, offset + 16));
        auto tail = entries_.mutableSpan(offset);
        std::memmove(tail.data(), tail.data() + kEntryBytes, tail.size() - kEntryBytes);
        entries_.trimTail(kEntryBytes);
        return result;
    }
    return std::nullopt;
}

} // namespace basalt::proxy::runtime::detail
