#pragma once

#include <basalt/proxy/config/RemoteConfig.hpp>
#include <basalt/proxy/runtime/CommandDispatcher.hpp>
#include <basalt/wire/remote/HttpServer.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime {
class ProxyRuntime;
}

namespace basalt::proxy::remote {

/// What a command run as the API produced: how the dispatcher took it and the plain text it replied with.
struct CommandReply {
    runtime::CommandOutcome outcome = runtime::CommandOutcome::kUnknown;
    std::vector<std::string> lines;
};

/// Everything the API reaches, handed in by the process that owns it. Every callback must be thread-safe.
struct RemoteHooks {
    runtime::ProxyRuntime* runtime = nullptr;
    /// Runs a command line through the proxy's command dispatcher as a console-like source.
    std::function<CommandReply(std::string_view line)> command;
    /// True when a Java or Bedrock player of that name is online.
    std::function<bool(std::string_view player)> playerOnline;
    std::function<std::size_t()> bedrockPlaying;
    std::function<std::size_t()> loadedPlugins;
};

/// The v1 administration API: read-only state endpoints and a few actions, behind a bearer token.
class RemoteApi {
public:
    /// Generates the token file on first start; nothing is started when the API is disabled.
    [[nodiscard]] static std::expected<std::unique_ptr<RemoteApi>, std::string>
    start(const config::RemoteConfig& settings, RemoteHooks hooks, const std::string& tokenPath);

    ~RemoteApi();
    RemoteApi(const RemoteApi&) = delete;
    RemoteApi& operator=(const RemoteApi&) = delete;

    /// Authentication already happened; also what the tests drive without a socket.
    [[nodiscard]] wire::remote::HttpReply handle(const wire::remote::http::Request& request, std::string_view peer);

    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    struct Impl;
    explicit RemoteApi(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace basalt::proxy::remote
