#pragma once

#include <basalt/wire/wasm/Abi.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace basalt::proxy::plugins {

inline constexpr std::size_t kMaxHandleSlots = 65535;

/// Slots of one handle type: a handle is generation << 16 | (index + 1), so a reused slot never answers an old handle.
template <typename T>
class HandleTable {
public:
    /// The new handle, or 0 when the table is full.
    [[nodiscard]] std::int32_t add(T value) {
        std::size_t index = 0;
        if (!free_.empty()) {
            index = *free_.begin();
            free_.erase(free_.begin());
        } else {
            if (slots_.size() >= kMaxHandleSlots) {
                return 0;
            }
            index = slots_.size();
            slots_.emplace_back();
        }
        slots_[index].value = std::move(value);
        return encode(index, slots_[index].generation);
    }

    /// kOk with `out` set, kErrInvalid for a malformed handle, kErrStale for one that was removed.
    [[nodiscard]] std::int32_t find(std::int32_t handle, T*& out) {
        std::size_t index = 0;
        std::uint16_t generation = 0;
        if (!decode(handle, index, generation) || index >= slots_.size()) {
            return wire::wasm::kErrInvalid;
        }
        Slot& slot = slots_[index];
        if (!slot.value.has_value() || slot.generation != generation) {
            return wire::wasm::kErrStale;
        }
        out = &*slot.value;
        return wire::wasm::kOk;
    }

    /// Whether the handle was live; its slot is free for reuse under a newer generation.
    bool remove(std::int32_t handle) {
        T* found = nullptr;
        if (find(handle, found) != wire::wasm::kOk) {
            return false;
        }
        release(static_cast<std::size_t>(static_cast<std::uint32_t>(handle) & 0xFFFFU) - 1);
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return slots_.size() - free_.size(); }

    template <typename Fn>
    void forEach(Fn&& fn) {
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (slots_[index].value.has_value()) {
                fn(encode(index, slots_[index].generation), *slots_[index].value);
            }
        }
    }

    /// Every handle becomes STALE.
    void clear() {
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (slots_[index].value.has_value()) {
                release(index);
            }
        }
    }

private:
    struct Slot {
        std::optional<T> value;
        std::uint16_t generation = 1;
    };

    static constexpr std::uint16_t kMaxGeneration = 0x7FFF;

    [[nodiscard]] static std::int32_t encode(std::size_t index, std::uint16_t generation) {
        return static_cast<std::int32_t>((static_cast<std::uint32_t>(generation) << 16U) |
                                         static_cast<std::uint32_t>(index + 1));
    }

    [[nodiscard]] static bool decode(std::int32_t handle, std::size_t& index, std::uint16_t& generation) {
        const auto low = static_cast<std::uint32_t>(handle) & 0xFFFFU;
        if (handle <= 0 || low == 0) {
            return false;
        }
        index = low - 1;
        generation = static_cast<std::uint16_t>(static_cast<std::uint32_t>(handle) >> 16U);
        return true;
    }

    void release(std::size_t index) {
        Slot& slot = slots_[index];
        slot.value.reset();
        slot.generation = slot.generation >= kMaxGeneration ? std::uint16_t{1} : std::uint16_t(slot.generation + 1);
        free_.insert(index);
    }

    std::vector<Slot> slots_;
    std::set<std::size_t> free_;
};

} // namespace basalt::proxy::plugins
