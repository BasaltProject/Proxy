#pragma once

#include <basalt/wire/text/LangTable.hpp>

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace basalt::proxy::runtime::detail {

/// Reads `en_us` from a Minecraft server jar or a bare language file; an empty path gives no table.
[[nodiscard]] std::expected<std::shared_ptr<const wire::text::LangTable>, std::string>
loadBedrockLanguage(std::string_view path);

} // namespace basalt::proxy::runtime::detail
