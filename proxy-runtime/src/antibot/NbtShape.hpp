#pragma once

#include <basalt/wire/nbt/NbtTag.hpp>

#include <cstddef>

namespace basalt::proxy::runtime::antibot {

struct NbtLimits {
    int depth = 24;
    std::size_t keys = 512;
    std::size_t bytes = 32768;
};

/// True when the tree stays inside the limits; the walk stops at the first excess, so it is bounded by them.
[[nodiscard]] bool nbtWithin(const wire::nbt::NbtTag& tag, const NbtLimits& limits);

} // namespace basalt::proxy::runtime::antibot
