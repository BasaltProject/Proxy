#include "Captcha.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace basalt::proxy::runtime::antibot {
namespace {
constexpr std::uint8_t kBackground = 8 * 4 + 2;
constexpr std::uint8_t kInk = 29 * 4 + 2;
constexpr std::array<std::uint8_t, 5> kNoise{21 * 4 + 1, 22 * 4 + 2, 11 * 4 + 1, 26 * 4 + 0, 28 * 4 + 1};
constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kScale = 4;

constexpr std::array<std::array<std::uint8_t, kGlyphHeight>, 10> kGlyphs{{
    {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110},
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111},
    {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110},
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010},
    {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110},
    {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110},
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000},
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110},
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100},
}};

void plot(CaptchaImage& image, int x, int y, std::uint8_t color) {
    if (x >= 0 && y >= 0 && x < kMapSide && y < kMapSide)
        image.colors[static_cast<std::size_t>(y * kMapSide + x)] = color;
}

void line(CaptchaImage& image, std::mt19937_64& rng, std::uint8_t color) {
    std::uniform_real_distribution<double> position(0, kMapSide - 1);
    const double x0 = 0, y0 = position(rng), x1 = kMapSide - 1, y1 = position(rng);
    const int steps = kMapSide * 2;
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const int x = static_cast<int>(x0 + (x1 - x0) * t), y = static_cast<int>(y0 + (y1 - y0) * t);
        plot(image, x, y, color);
        plot(image, x, y + 1, color);
    }
}
} // namespace

CaptchaImage CaptchaFactory::render(std::string_view digits, std::uint64_t seed) {
    CaptchaImage image;
    image.answer = std::string{digits};
    std::mt19937_64 rng{seed};
    image.colors.fill(kBackground);
    std::uniform_int_distribution<int> any(0, kMapSide - 1), jitter(-3, 3), shearOf(-2, 2), noise(0, 4);
    for (int i = 0; i < 900; ++i)
        plot(image, any(rng), any(rng), kNoise[static_cast<std::size_t>(noise(rng))]);
    const int width = kGlyphWidth * kScale, height = kGlyphHeight * kScale;
    const int count = static_cast<int>(digits.size());
    const int gap = std::max(2, (kMapSide - 8 - count * width) / std::max(1, count - 1));
    const int total = count * width + (count - 1) * gap;
    int left = (kMapSide - total) / 2;
    for (const char c : digits) {
        const auto& glyph = kGlyphs[static_cast<std::size_t>(c - '0')];
        const int top = (kMapSide - height) / 2 + jitter(rng) * 2;
        const int shear = shearOf(rng);
        for (int row = 0; row < kGlyphHeight; ++row)
            for (int column = 0; column < kGlyphWidth; ++column) {
                if (!(glyph[static_cast<std::size_t>(row)] & (1 << (kGlyphWidth - 1 - column))))
                    continue;
                for (int dy = 0; dy < kScale; ++dy)
                    for (int dx = 0; dx < kScale; ++dx) {
                        const int y = top + row * kScale + dy;
                        const int x = left + column * kScale + dx + shear * (row - kGlyphHeight / 2) / 3;
                        if (noise(rng) != 0 || dx == 0)
                            plot(image, x, y, kInk);
                    }
            }
        left += width + gap + jitter(rng) / 2;
    }
    for (int i = 0; i < 5; ++i)
        line(image, rng, i % 2 ? kInk : kNoise[static_cast<std::size_t>(noise(rng))]);
    return image;
}

BlockCaptcha CaptchaFactory::build(std::string_view digits, std::uint64_t seed) {
    constexpr std::int32_t kBackdrop = 17026, kRed = 17025, kOrange = 17012;
    constexpr std::array<std::int32_t, 3> kInks{17011, 17016, 17015};
    constexpr int kCentreX = 8, kBaseY = 64, kRows = 10;
    std::mt19937_64 rng{seed};
    const auto ink = kInks[rng() % kInks.size()];
    BlockCaptcha result;
    result.answer = std::string{digits};
    const int count = static_cast<int>(digits.size());
    const int width = count * kGlyphWidth + (count - 1);
    const int left = kCentreX - width / 2;
    std::vector<std::int32_t> grid(static_cast<std::size_t>((width + 2) * kRows), kBackdrop);
    const auto at = [&](int column, int row) -> std::int32_t& {
        return grid[static_cast<std::size_t>(row * (width + 2) + column + 1)];
    };
    std::vector<bool> used(grid.size(), false);
    for (int k = 0; k < count; ++k) {
        const auto& glyph = kGlyphs[static_cast<std::size_t>(digits[static_cast<std::size_t>(k)] - '0')];
        const int lift = 1 + static_cast<int>(rng() % 3) - 1;
        for (int row = 0; row < kGlyphHeight; ++row)
            for (int column = 0; column < kGlyphWidth; ++column)
                if (glyph[static_cast<std::size_t>(row)] & (1 << (kGlyphWidth - 1 - column))) {
                    const int cx = k * (kGlyphWidth + 1) + column, cy = kRows - 2 - row - lift + 1;
                    at(cx, cy) = ink;
                    used[static_cast<std::size_t>(cy * (width + 2) + cx + 1)] = true;
                }
    }
    for (int i = 0; i < width * 2 / 3; ++i) {
        const int cx = static_cast<int>(rng() % static_cast<std::uint64_t>(width + 2)) - 1;
        const int cy = static_cast<int>(rng() % kRows);
        if (!used[static_cast<std::size_t>(cy * (width + 2) + cx + 1)])
            at(cx, cy) = (rng() & 1) ? kRed : kOrange;
    }
    for (int row = 0; row < kRows; ++row)
        for (int column = -1; column <= width; ++column)
            result.blocks.push_back({left + column, kBaseY + row, kWallZ, at(column, row)});
    return result;
}

std::unique_ptr<BlockCaptcha> CaptchaFactory::makeBlocks() const {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::string code;
    for (std::int32_t i = 0; i < length_; ++i)
        code.push_back(static_cast<char>('0' + rng() % 10));
    return std::make_unique<BlockCaptcha>(build(code, rng()));
}

std::unique_ptr<CaptchaImage> CaptchaFactory::make() const {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::string code;
    for (std::int32_t i = 0; i < length_; ++i)
        code.push_back(static_cast<char>('0' + rng() % 10));
    return std::make_unique<CaptchaImage>(render(code, rng()));
}

} // namespace basalt::proxy::runtime::antibot
