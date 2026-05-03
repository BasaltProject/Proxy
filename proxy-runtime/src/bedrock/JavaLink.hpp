#pragma once

#include "../BackendDial.hpp"
#include "../BackendLogin.hpp"

#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/wire/auth/SessionProfile.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace basalt::proxy::runtime::detail {

/// The proxy as a Java client: it logs a Bedrock player in to a native backend, walks the configuration and then hands
/// every play packet to its owner, answering the ones that are only about keeping the connection alive.
struct NativeForwarding;

class JavaLink {
public:
    enum class State { kIdle, kConnecting, kLogin, kConfiguration, kPlay, kClosed };

    struct Events {
        std::function<void(std::int32_t id, std::span<const std::uint8_t> body)> play;
        std::function<void(const std::string& registry, std::vector<std::string> keys)> registry;
        std::function<void(const std::string& reason)> closed;
    };

    JavaLink(config::BackendConfig backend, wire::auth::SessionProfile profile, std::string clientAddress,
             std::uint16_t clientPort, Events events, const NativeForwarding* native = nullptr,
             std::string originId = {});

    /// Resolves the backend and starts connecting; false when it cannot be reached at all.
    [[nodiscard]] bool begin(std::chrono::milliseconds budget);

    /// Moves everything forward without waiting.
    void pump();

    /// A packet for the backend in play: the id, then the body.
    void send(std::int32_t id, std::span<const std::uint8_t> body);

    [[nodiscard]] State state() const noexcept { return state_; }

    void close(const std::string& reason);

private:
    void handle(std::span<const std::uint8_t> packet);
    void handleLogin(std::int32_t id, wire::io::ByteBuffer& body);
    void handleConfiguration(std::int32_t id, wire::io::ByteBuffer& body);
    void handlePlay(std::int32_t id, wire::io::ByteBuffer& body);
    void sendFrame(std::int32_t id, std::span<const std::uint8_t> body);

    config::BackendConfig backend_;
    wire::auth::SessionProfile profile_;
    std::string clientAddress_;
    std::uint16_t clientPort_ = 0;
    Events events_;
    const NativeForwarding* native_ = nullptr;
    std::string originId_;
    State state_ = State::kIdle;
    std::unique_ptr<BackendDial> dial_;
    std::unique_ptr<BackendLogin> login_;
    wire::io::ByteBuffer scratch_;
};

} // namespace basalt::proxy::runtime::detail
