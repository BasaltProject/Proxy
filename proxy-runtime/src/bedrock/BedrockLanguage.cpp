#include "BedrockLanguage.hpp"

#include <basalt/wire/io/FileIO.hpp>
#include <basalt/wire/io/ZipReader.hpp>

#include <cstdint>
#include <vector>

namespace basalt::proxy::runtime::detail {
namespace {

constexpr std::string_view kEntry = "assets/minecraft/lang/en_us.json";

} // namespace

std::expected<std::shared_ptr<const wire::text::LangTable>, std::string> loadBedrockLanguage(std::string_view path) {
    if (path.empty())
        return nullptr;
    const std::string name{path};
    const auto bytes = wire::io::fileIO::readBytes(name);
    if (!bytes)
        return std::unexpected("bedrock.language: " + name + " cannot be read");
    std::vector<std::uint8_t> json;
    if (name.ends_with(".json")) {
        json = *bytes;
    } else {
        const auto jar = wire::io::ZipReader::open(*bytes);
        if (!jar)
            return std::unexpected("bedrock.language: " + name + " is neither a jar nor a .json file");
        auto entry = jar->read(std::string{kEntry});
        if (!entry)
            return std::unexpected("bedrock.language: " + name + " has no " + std::string{kEntry});
        json = std::move(*entry);
    }
    auto table = wire::text::parseLangTable({reinterpret_cast<const char*>(json.data()), json.size()});
    if (!table)
        return std::unexpected("bedrock.language: " + name + " is not a language file");
    return std::make_shared<const wire::text::LangTable>(std::move(*table));
}

} // namespace basalt::proxy::runtime::detail
