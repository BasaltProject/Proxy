#pragma once

#include <string_view>

namespace basalt::proxy::plugins {

enum class MappingStatus {
    /// The native and WASM surface exists and works today.
    kNative,
    /// The native surface exists but does not cover the whole contract of the foreign API.
    kPartial,
    /// The native and WASM equivalent has to be designed and built before the adapter can map it.
    kNeedsNative,
};

/// One row of an adapter's table: a foreign API member, the native service behind it and its WASM import.
struct AdapterMapping {
    std::string_view foreign;
    std::string_view native;
    std::string_view wasm;
    MappingStatus status;
};

} // namespace basalt::proxy::plugins
