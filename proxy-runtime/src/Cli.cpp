#include <basalt/proxy/runtime/BuildInfo.hpp>
#include <basalt/proxy/runtime/Cli.hpp>

namespace basalt::proxy::runtime::cli {

CliCommand parse(const std::vector<std::string_view>& args) {
    std::string configDirectory{kDefaultConfigDirectory};
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string_view arg = args[index];
        if (arg == "-h" || arg == "--help") {
            return cliCommand::ShowHelp{};
        }
        if (arg == "-v" || arg == "--version") {
            return cliCommand::ShowVersion{};
        }
        if (arg == "--config") {
            if (index + 1 >= args.size()) {
                return cliCommand::Invalid{.message = "--config needs a directory, for example --config ./config"};
            }
            const std::string_view value = args[index + 1];
            // A following flag means the value was forgotten, not that a directory is named "--port".
            if (value.starts_with('-')) {
                return cliCommand::Invalid{.message = "--config needs a directory, but was followed by '" +
                                                      std::string{value} + "'"};
            }
            configDirectory = std::string{value};
            ++index;
            continue;
        }
        return cliCommand::Invalid{.message = "Unknown option '" + std::string{arg} +
                                              "'. Run with --help to see what is accepted."};
    }
    return cliCommand::Run{.configDirectory = configDirectory};
}

std::string helpText() {
    return "Basalt Proxy " + std::string{buildInfo::kVersion} +
           "\n"
           "\n"
           "Usage: basalt-proxy [options]\n"
           "\n"
           "Options:\n"
           "  --config <dir>   Directory to read configuration from (default: " +
           std::string{kDefaultConfigDirectory} +
           ")\n"
           "  -v, --version    Print the version and exit\n"
           "  -h, --help       Print this help and exit";
}

std::string versionLine() {
    return "Basalt Proxy " + std::string{buildInfo::kVersion} + " (Minecraft " +
           std::string{buildInfo::kMinecraftVersion} + ", protocol " + std::to_string(buildInfo::kProtocolVersion) +
           ")";
}

} // namespace basalt::proxy::runtime::cli
