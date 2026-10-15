#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <basalt/proxy/runtime/Cli.hpp>

#include <doctest.h>

using namespace basalt::proxy::runtime;

TEST_CASE("no arguments runs with the default configuration directory") {
    const CliCommand parsed = cli::parse({});
    const auto* run = std::get_if<cliCommand::Run>(&parsed);
    REQUIRE(run != nullptr);
    CHECK(run->configDirectory == cli::kDefaultConfigDirectory);
}

TEST_CASE("a config without a value is rejected rather than defaulted") {
    CHECK(std::holds_alternative<cliCommand::Invalid>(cli::parse({"--config"})));
    CHECK(std::holds_alternative<cliCommand::Invalid>(cli::parse({"--config", "--version"})));
}
