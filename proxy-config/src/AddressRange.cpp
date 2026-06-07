#include <basalt/proxy/config/AddressRange.hpp>

#include <algorithm>
#include <charconv>
#include <vector>

namespace basalt::proxy::config {
namespace {
constexpr std::array<std::uint8_t, 12> kMappedPrefix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};

std::optional<std::array<std::uint8_t, 4>> parseV4(std::string_view text) {
    std::array<std::uint8_t, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto dot = text.find('.');
        if ((i < 3) != (dot != std::string_view::npos))
            return std::nullopt;
        const auto part = text.substr(0, dot);
        unsigned value = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || part.size() > 3 || error != std::errc{} || end != part.data() + part.size() || value > 255)
            return std::nullopt;
        out[i] = static_cast<std::uint8_t>(value);
        if (dot != std::string_view::npos)
            text.remove_prefix(dot + 1);
    }
    return out;
}

std::optional<std::vector<std::uint16_t>> parseGroups(std::string_view text) {
    std::vector<std::uint16_t> groups;
    while (!text.empty()) {
        const auto colon = text.find(':');
        const auto part = text.substr(0, colon);
        if (colon == std::string_view::npos && part.find('.') != std::string_view::npos) {
            const auto v4 = parseV4(part);
            if (!v4)
                return std::nullopt;
            groups.push_back(static_cast<std::uint16_t>((*v4)[0] << 8 | (*v4)[1]));
            groups.push_back(static_cast<std::uint16_t>((*v4)[2] << 8 | (*v4)[3]));
            return groups;
        }
        unsigned value = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value, 16);
        if (part.empty() || part.size() > 4 || error != std::errc{} || end != part.data() + part.size())
            return std::nullopt;
        groups.push_back(static_cast<std::uint16_t>(value));
        if (colon == std::string_view::npos)
            break;
        text.remove_prefix(colon + 1);
        if (text.empty())
            return std::nullopt;
    }
    return groups;
}

std::optional<AddressRange::Bytes> parseV6(std::string_view text) {
    std::vector<std::uint16_t> head;
    std::vector<std::uint16_t> tail;
    const auto gap = text.find("::");
    if (gap == std::string_view::npos) {
        auto all = parseGroups(text);
        if (!all || all->size() != 8)
            return std::nullopt;
        head = std::move(*all);
    } else {
        if (text.find("::", gap + 1) != std::string_view::npos)
            return std::nullopt;
        auto first = parseGroups(text.substr(0, gap));
        auto second = parseGroups(text.substr(gap + 2));
        if (!first || !second || first->size() + second->size() > 7)
            return std::nullopt;
        head = std::move(*first);
        tail = std::move(*second);
        head.resize(8 - tail.size(), 0);
    }
    head.insert(head.end(), tail.begin(), tail.end());
    AddressRange::Bytes out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 2] = static_cast<std::uint8_t>(head[i] >> 8);
        out[i * 2 + 1] = static_cast<std::uint8_t>(head[i] & 0xFF);
    }
    return out;
}
} // namespace

std::optional<AddressRange::Bytes> AddressRange::parseAddress(std::string_view text) {
    Bytes out{};
    if (text.find(':') == std::string_view::npos) {
        const auto v4 = parseV4(text);
        if (!v4)
            return std::nullopt;
        std::ranges::copy(kMappedPrefix, out.begin());
        std::ranges::copy(*v4, out.begin() + 12);
        return out;
    }
    auto v6 = parseV6(text);
    return v6;
}

std::optional<AddressRange> AddressRange::parse(std::string_view text) {
    const auto slash = text.find('/');
    const auto address = parseAddress(text.substr(0, slash));
    if (!address)
        return std::nullopt;
    const bool v4 = text.substr(0, slash).find(':') == std::string_view::npos;
    unsigned prefix = v4 ? 32 : 128;
    if (slash != std::string_view::npos) {
        const auto digits = text.substr(slash + 1);
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), prefix);
        if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size() ||
            prefix > (v4 ? 32U : 128U))
            return std::nullopt;
    }
    AddressRange range;
    range.base_ = *address;
    range.prefixBits_ = static_cast<std::uint8_t>(v4 ? prefix + 96 : prefix);
    return range;
}

bool AddressRange::contains(const Bytes& address) const noexcept {
    const std::size_t whole = prefixBits_ / 8;
    if (!std::equal(base_.begin(), base_.begin() + whole, address.begin()))
        return false;
    const unsigned rest = prefixBits_ % 8;
    if (rest == 0)
        return true;
    const auto mask = static_cast<std::uint8_t>(0xFF << (8 - rest));
    return (base_[whole] & mask) == (address[whole] & mask);
}

} // namespace basalt::proxy::config
