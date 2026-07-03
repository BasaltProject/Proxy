#include "NbtShape.hpp"

namespace basalt::proxy::runtime::antibot {
namespace {
using wire::nbt::NbtCompound;
using wire::nbt::NbtList;
using wire::nbt::NbtTag;

struct Walk {
    const NbtLimits& limits;
    std::size_t keys = 0;
    std::size_t bytes = 0;
    std::size_t nodes = 0;

    bool visit(const NbtTag& tag, int depth) {
        if (depth > limits.depth || ++nodes > limits.keys * 8 + 64)
            return false;
        if (const auto* compound = tag.get<NbtCompound>()) {
            keys += compound->size();
            if (keys > limits.keys)
                return false;
            for (std::size_t i = 0; i < compound->size(); ++i) {
                bytes += compound->names()[i].size();
                if (bytes > limits.bytes || !visit(compound->values()[i], depth + 1))
                    return false;
            }
        } else if (const auto* list = tag.get<NbtList>()) {
            for (const auto& element : list->elements())
                if (!visit(element, depth + 1))
                    return false;
        } else if (const auto* text = tag.get<std::string>()) {
            bytes += text->size();
        } else if (const auto* bytesArray = tag.get<std::vector<std::int8_t>>()) {
            bytes += bytesArray->size();
        } else if (const auto* ints = tag.get<std::vector<std::int32_t>>()) {
            bytes += ints->size() * 4;
        } else if (const auto* longs = tag.get<std::vector<std::int64_t>>()) {
            bytes += longs->size() * 8;
        }
        return bytes <= limits.bytes;
    }
};
} // namespace

bool nbtWithin(const NbtTag& tag, const NbtLimits& limits) { return Walk{limits}.visit(tag, 1); }

} // namespace basalt::proxy::runtime::antibot
