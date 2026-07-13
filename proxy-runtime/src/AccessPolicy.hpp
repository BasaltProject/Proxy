#pragma once

#include "antibot/Antibot.hpp"
#include "antibot/LimboAssets.hpp"

#include <basalt/proxy/config/AddressRange.hpp>
#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/Admission.hpp>
#include <basalt/wire/io/ConnectionGate.hpp>
#include <basalt/wire/protocol/PacketLimiter.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace basalt::proxy::runtime::detail {

/// The reloadable access settings. A session keeps the snapshot it was created with.
struct AccessSettings {
    static std::shared_ptr<const AccessSettings> make(const config::ProxyConfig& config);
    ~AccessSettings();

    config::NetworkConfig network;
    config::GeneralConfig general;
    config::LimitsConfig limits;
    std::vector<config::AddressRange> trusted;
    std::shared_ptr<const wire::protocol::PacketLimits> packetLimits;

    [[nodiscard]] bool trustsUpstream(std::string_view address) const;
    [[nodiscard]] bool allowsName(std::string_view name) const;

private:
    AccessSettings();
    struct Filter;
    std::unique_ptr<Filter> usernames_;
};

/// Who may connect and what a connection may do, shared by every session owner.
class AccessPolicy {
public:
    AccessPolicy(const config::ProxyConfig& config, Admission& admission);

    Admission& admission;
    mutable wire::io::ConnectionGate gate;
    mutable antibot::Antibot antibot;
    mutable antibot::LimboAssets limbo;

    [[nodiscard]] std::shared_ptr<const AccessSettings> settings() const;
    void apply(const config::ProxyConfig& config);

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const AccessSettings> settings_;
};

/// What a session is handed when it is created: the policy, its settings and the address slot when known.
struct ClientAccess {
    const AccessPolicy* policy = nullptr;
    std::shared_ptr<const AccessSettings> settings;
    std::optional<wire::io::ConnectionGate::Slot> slot;
    bool awaitHeader = false;
    std::function<bool(std::string_view)> isOperator;
};

} // namespace basalt::proxy::runtime::detail
