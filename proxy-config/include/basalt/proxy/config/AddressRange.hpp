#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace basalt::proxy::config {

/// A literal IPv4 or IPv6 address with an optional prefix length; IPv4-mapped IPv6 is read as plain IPv4.
class AddressRange {
public:
    using Bytes = std::array<std::uint8_t, 16>;

    [[nodiscard]] static std::optional<AddressRange> parse(std::string_view text);
    [[nodiscard]] static std::optional<Bytes> parseAddress(std::string_view text);
    [[nodiscard]] static std::vector<AddressRange> parseList(std::string_view text);

    [[nodiscard]] bool contains(const Bytes& address) const noexcept;

private:
    Bytes base_{};
    std::uint8_t prefixBits_ = 128;
};

} // namespace basalt::proxy::config
