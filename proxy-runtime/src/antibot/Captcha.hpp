#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime::antibot {

inline constexpr std::int32_t kMapSide = 128;

struct CaptchaImage {
    std::string answer;
    std::array<std::uint8_t, kMapSide * kMapSide> colors{};
};

struct CaptchaBlock {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::int32_t state = 0;
};

/// A number built of blocks on a wall in front of the limbo spawn, with loose blocks around it.
struct BlockCaptcha {
    std::string answer;
    std::vector<CaptchaBlock> blocks;
};

inline constexpr std::int32_t kWallZ = 20;

/// Draws a new picture for every player, so none can be learned and the answers never repeat.
class CaptchaFactory {
public:
    explicit CaptchaFactory(std::int32_t length) : length_(length) {}

    [[nodiscard]] std::unique_ptr<CaptchaImage> make() const;

    [[nodiscard]] std::int32_t length() const noexcept { return length_; }

    [[nodiscard]] std::unique_ptr<BlockCaptcha> makeBlocks() const;
    [[nodiscard]] static BlockCaptcha build(std::string_view digits, std::uint64_t seed);
    [[nodiscard]] static CaptchaImage render(std::string_view digits, std::uint64_t seed);

private:
    std::int32_t length_;
};

} // namespace basalt::proxy::runtime::antibot
