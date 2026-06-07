#pragma once

#include <basalt/proxy/config/RemoteConfig.hpp>
#include <basalt/wire/remote/HttpServer.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace basalt::proxy::runtime {
class ProxyRuntime;
}

namespace basalt::proxy::remote {

/// Everything the API reaches, handed in by the process that owns it. Every callback must be thread-safe.
struct RemoteHooks {
    runtime::ProxyRuntime* runtime = nullptr;
    /// Reloads the configuration files; the message says what changed or why nothing did.
    std::function<std::expected<std::string, std::string>()> reload;
    std::function<std::size_t()> bedrockPlaying;
    /// True when the player is a Bedrock player and the change was requested.
    std::function<bool(std::string_view player, std::string_view backend)> bedrockSend;
    std::function<void(std::string_view player, std::string_view reason)> bedrockKick;
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
