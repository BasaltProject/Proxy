#pragma once

#include <basalt/wire/common/json/Json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::remote::json {

using wire::common::JsonObject;
using wire::common::JsonValue;

[[nodiscard]] inline JsonValue str(std::string_view text) { return JsonValue::of(std::string{text}); }

[[nodiscard]] inline JsonValue num(std::uint64_t value) { return JsonValue::of(static_cast<std::int64_t>(value)); }

[[nodiscard]] inline JsonValue flag(bool value) { return JsonValue::of(value); }

[[nodiscard]] inline JsonValue obj(JsonObject value) { return JsonValue::ofObject(std::move(value)); }

[[nodiscard]] inline JsonValue list(std::vector<JsonValue> values) { return JsonValue::ofArray(std::move(values)); }

[[nodiscard]] inline JsonValue null() { return JsonValue{}; }

[[nodiscard]] inline std::string text(JsonObject value) { return wire::common::writeJson(obj(std::move(value))); }

/// The string at `key` of a request body object, or empty when the body is not an object or lacks it.
[[nodiscard]] std::string stringField(std::string_view body, std::string_view key, bool& wellFormed);

} // namespace basalt::proxy::remote::json
