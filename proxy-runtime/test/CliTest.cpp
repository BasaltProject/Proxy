#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <basalt/proxy/runtime/Cli.hpp>

#include <doctest.h>

#include <string>

using namespace basalt::proxy::runtime;

TEST_CASE("no arguments runs with the default configuration directory") {
    const CliCommand parsed = cli::parse({});
    const auto* run = std::get_if<cliCommand::Run>(&parsed);
    REQUIRE(run != nullptr);
    CHECK(run->configDirectory == cli::kDefaultConfigDirectory);
}

TEST_CASE("help and version win over anything after them") {
    CHECK(std::holds_alternative<cliCommand::ShowHelp>(cli::parse({"--help", "--config", "x"})));
    CHECK(std::holds_alternative<cliCommand::ShowHelp>(cli::parse({"-h"})));
    CHECK(std::holds_alternative<cliCommand::ShowVersion>(cli::parse({"--version"})));
    CHECK(std::holds_alternative<cliCommand::ShowVersion>(cli::parse({"-v"})));
}

TEST_CASE("config takes the following argument") {
    const CliCommand parsed = cli::parse({"--config", "/etc/basalt"});
    const auto* run = std::get_if<cliCommand::Run>(&parsed);
    REQUIRE(run != nullptr);
    CHECK(run->configDirectory == "/etc/basalt");
}

TEST_CASE("a config without a value is rejected rather than defaulted") {
    CHECK(std::holds_alternative<cliCommand::Invalid>(cli::parse({"--config"})));
    CHECK(std::holds_alternative<cliCommand::Invalid>(cli::parse({"--config", "--version"})));
}

TEST_CASE("an unknown option names itself") {
    const CliCommand parsed = cli::parse({"--nope"});
    const auto* invalid = std::get_if<cliCommand::Invalid>(&parsed);
    REQUIRE(invalid != nullptr);
    CHECK(invalid->message.find("--nope") != std::string::npos);
}

TEST_CASE("help lists every option it accepts") {
    const std::string help = cli::helpText();
    for (const std::string_view option : {"--config", "--version", "--help"}) {
        CHECK(help.find(option) != std::string::npos);
    }
}
