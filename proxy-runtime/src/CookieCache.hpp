#pragma once

#include <basalt/wire/io/ByteBuffer.hpp>

#include <optional>
#include <span>
#include <string_view>

namespace basalt::proxy::runtime::detail {

struct CachedCookie {
    bool known = false;
    std::optional<std::span<const std::uint8_t>> payload;
    std::uint64_t storeRevision = 0;
};

/// Session-local values observed on the connection, including known absence. No identity authority.
class CookieCache {
public:
    explicit CookieCache(std::pmr::memory_resource& memory) : values_(0, memory) {}

    [[nodiscard]] bool remember(std::string_view key, std::optional<std::span<const std::uint8_t>> payload);
    [[nodiscard]] bool store(std::string_view key, std::span<const std::uint8_t> payload);
    /// Views expire at the next mutation; never pass them back to remember.
    [[nodiscard]] CachedCookie lookup(std::string_view key) const;

    [[nodiscard]] std::size_t allocated() const { return values_.capacity(); }

private:
    struct Entry {
        std::size_t offset;
        std::size_t size;
        std::size_t payloadOffset;
        std::size_t payloadSize;
        bool present;
        std::uint64_t storeRevision;
    };

    [[nodiscard]] std::optional<Entry> find(std::string_view key) const;
    [[nodiscard]] bool update(std::string_view key, std::optional<std::span<const std::uint8_t>> payload, bool stored);
    wire::io::ByteBuffer values_;
    std::uint64_t storeRevision_ = 0;
    std::size_t count_ = 0;
};

} // namespace basalt::proxy::runtime::detail
