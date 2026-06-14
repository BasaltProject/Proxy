#include <basalt/proxy/plugins/JarDetector.hpp>
#include <basalt/wire/io/ZipReader.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace basalt::proxy::plugins {

ForeignKind detectJar(std::string_view path) {
    std::ifstream file{std::string{path}, std::ios::binary};
    if (!file)
        return ForeignKind::kNone;
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    auto archive = wire::io::ZipReader::open(std::move(bytes));
    if (!archive)
        return ForeignKind::kNone;
    if (archive->contains("velocity-plugin.json"))
        return ForeignKind::kVelocity;
    if (archive->contains("bungee.yml") || archive->contains("plugin.yml"))
        return ForeignKind::kBungee;
    return ForeignKind::kNone;
}

std::vector<DetectedJar> detectJars(std::string_view directory) {
    std::vector<DetectedJar> found;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{std::string{directory}, error}) {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".jar")
            continue;
        const auto kind = detectJar(entry.path().string());
        if (kind != ForeignKind::kNone)
            found.push_back({entry.path().string(), kind});
    }
    std::ranges::sort(found, {}, &DetectedJar::path);
    return found;
}

} // namespace basalt::proxy::plugins
