#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace basalt::proxy::runtime {

/// What the command line asked for. Parsing never exits or prints: `main` decides both.
namespace cliCommand {

struct ShowHelp {};

struct ShowVersion {};

/// Start the proxy, reading its configuration from `configDirectory`.
struct Run {
    std::string configDirectory;

    friend bool operator==(const Run&, const Run&) = default;
};

/// The arguments did not parse. `message` is one line, already addressed to an operator.
struct Invalid {
    std::string message;

    friend bool operator==(const Invalid&, const Invalid&) = default;
};

} // namespace cliCommand

using CliCommand = std::variant<cliCommand::ShowHelp, cliCommand::ShowVersion, cliCommand::Run, cliCommand::Invalid>;

namespace cli {

inline constexpr std::string_view kDefaultConfigDirectory = "./config";

[[nodiscard]] CliCommand parse(const std::vector<std::string_view>& args);

[[nodiscard]] std::string helpText();

[[nodiscard]] std::string versionLine();

} // namespace cli

} // namespace basalt::proxy::runtime
