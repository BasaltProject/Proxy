#include "DefaultIcon.hpp"

#include <basalt/wire/io/Compression.hpp>

#include <array>
#include <cmath>
#include <cstddef>

namespace basalt::proxy::runtime::detail {
namespace {

constexpr int kSize = 64;
constexpr double kCell = 9.0;

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0xFFFFFFFFU) {
    static const std::array<std::uint32_t, 256> kTable = [] {
        std::array<std::uint32_t, 256> built{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1U) != 0 ? 0xEDB88320U ^ (value >> 1) : value >> 1;
            built[index] = value;
        }
        return built;
    }();
    for (std::size_t index = 0; index < size; ++index)
        crc = kTable[(crc ^ data[index]) & 0xFFU] ^ (crc >> 8);
    return crc;
}

void putInt(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void chunk(std::vector<std::uint8_t>& out, const char (&type)[5], const std::vector<std::uint8_t>& body) {
    putInt(out, static_cast<std::uint32_t>(body.size()));
    std::vector<std::uint8_t> checked{type, type + 4};
    checked.insert(checked.end(), body.begin(), body.end());
    out.insert(out.end(), checked.begin(), checked.end());
    putInt(out, ~crc32(checked.data(), checked.size()));
}

std::array<std::uint8_t, 3> pixel(int x, int y) {
    const double px = x + 0.5;
    const double py = y + 0.5;
    const double q = (std::sqrt(3.0) / 3.0 * px - py / 3.0) / kCell;
    const double r = (2.0 / 3.0 * py) / kCell;
    double rq = std::round(q);
    double rr = std::round(r);
    const double rs = std::round(-q - r);
    const double dq = std::abs(rq - q);
    const double dr = std::abs(rr - r);
    const double ds = std::abs(rs + q + r);
    if (dq > dr && dq > ds)
        rq = -rr - rs;
    else if (dr > ds)
        rr = -rq - rs;
    const double cx = kCell * std::sqrt(3.0) * (rq + rr / 2.0);
    const double cy = kCell * 1.5 * rr;
    const double distance = std::hypot(px - cx, py - cy) / kCell;
    const auto cellHash = static_cast<std::uint32_t>(static_cast<int>(rq) * 73856093 ^ static_cast<int>(rr) * 19349663);
    const int tone = 38 + static_cast<int>(cellHash % 14U) - static_cast<int>(distance * 10.0);
    const int shade = distance > 0.82 ? 22 : tone;
    return {static_cast<std::uint8_t>(shade), static_cast<std::uint8_t>(shade + 2),
            static_cast<std::uint8_t>(shade + 6)};
}

} // namespace

std::vector<std::uint8_t> defaultIcon() {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(kSize) * (kSize * 3 + 1));
    for (int y = 0; y < kSize; ++y) {
        raw.push_back(0);
        for (int x = 0; x < kSize; ++x)
            for (const std::uint8_t channel : pixel(x, y))
                raw.push_back(channel);
    }
    auto packed = wire::io::zlib::compress(raw, 9);
    if (!packed)
        return {};

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> header;
    putInt(header, kSize);
    putInt(header, kSize);
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", *packed);
    chunk(png, "IEND", {});
    return png;
}

} // namespace basalt::proxy::runtime::detail
