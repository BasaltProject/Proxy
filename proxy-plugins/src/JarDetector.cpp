#include <basalt/proxy/plugins/JarDetector.hpp>
#include <basalt/wire/io/ZipReader.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace basalt::proxy::plugins {
namespace {

std::string lowercase(std::string_view text) {
    std::string result{text};
    std::ranges::transform(result, result.begin(),
                           [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
    return result;
}

std::string entryText(const wire::io::ZipReader& archive, std::string_view name) {
    auto bytes = archive.read(name);
    return bytes ? std::string{bytes->begin(), bytes->end()} : std::string{};
}

std::string unquote(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r' || value.back() == '\t'))
        value.remove_suffix(1);
    if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"') && value.back() == value.front())
        value = value.substr(1, value.size() - 2);
    return std::string{value};
}

std::string velocityId(std::string_view json) {
    const auto key = json.find("\"id\"");
    if (key == std::string_view::npos)
        return {};
    const auto open = json.find('"', json.find(':', key));
    const auto close = open == std::string_view::npos ? open : json.find('"', open + 1);
    return close == std::string_view::npos ? std::string{} : std::string{json.substr(open + 1, close - open - 1)};
}

std::string yamlName(std::string_view yaml) {
    for (std::size_t start = 0; start < yaml.size();) {
        const auto end = std::min(yaml.find('\n', start), yaml.size());
        const auto line = yaml.substr(start, end - start);
        if (line.starts_with("name:"))
            return unquote(line.substr(5));
        start = end + 1;
    }
    return {};
}

} // namespace

ForeignKind detectJar(std::string_view path, const PlatformChoices& choices) {
    std::ifstream file{std::string{path}, std::ios::binary};
    if (!file)
        return ForeignKind::kNone;
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    auto archive = wire::io::ZipReader::open(std::move(bytes));
    if (!archive)
        return ForeignKind::kNone;
    const bool velocity = archive->contains("velocity-plugin.json");
    const char* bungeeEntry = archive->contains("bungee.yml")   ? "bungee.yml"
                              : archive->contains("plugin.yml") ? "plugin.yml"
                                                                : nullptr;
    if (velocity && bungeeEntry && !choices.empty()) {
        const std::string keys[] = {lowercase(velocityId(entryText(*archive, "velocity-plugin.json"))),
                                    lowercase(yamlName(entryText(*archive, bungeeEntry))),
                                    lowercase(std::filesystem::path{std::string{path}}.filename().string())};
        for (const auto& key : keys)
            if (const auto found = choices.find(key); !key.empty() && found != choices.end())
                return found->second;
    }
    if (velocity)
        return ForeignKind::kVelocity;
    return bungeeEntry ? ForeignKind::kBungee : ForeignKind::kNone;
}

std::vector<DetectedJar> detectJars(std::string_view directory, const PlatformChoices& choices) {
    std::vector<DetectedJar> found;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{std::string{directory}, error}) {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".jar")
            continue;
        const auto kind = detectJar(entry.path().string(), choices);
        if (kind != ForeignKind::kNone)
            found.push_back({entry.path().string(), kind});
    }
    std::ranges::sort(found, {}, &DetectedJar::path);
    return found;
}

std::expected<std::filesystem::path, std::string> stageJars(const std::vector<DetectedJar>& jars, ForeignKind kind,
                                                            const std::filesystem::path& facadeRoot) {
    namespace fs = std::filesystem;
    const fs::path folder = facadeRoot / kStagedJarsFolder;
    std::error_code error;
    fs::remove_all(folder, error);
    if (!fs::create_directories(folder, error) && error)
        return std::unexpected("could not create " + folder.string() + ": " + error.message());
    for (const DetectedJar& jar : jars) {
        if (jar.kind != kind)
            continue;
        const fs::path source{jar.path};
        const fs::path target = folder / source.filename();
        fs::create_hard_link(source, target, error);
        if (error && !fs::copy_file(source, target, fs::copy_options::overwrite_existing, error))
            return std::unexpected("could not stage " + source.string() + ": " + error.message());
    }
    return fs::absolute(folder);
}

} // namespace basalt::proxy::plugins
