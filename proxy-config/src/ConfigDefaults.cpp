#include <basalt/proxy/config/ProxyConfig.hpp>

namespace basalt::proxy::config {

std::string_view defaultProxyToml() noexcept {
    return R"(# Public listener and authentication. Backends are configured in backends.toml.
[network]
# Literal IPv4 or IPv6 bind address. IPv6 dual-stack must be requested explicitly.
bind = "0.0.0.0"
port = 25565
dual-stack = false
# Authenticate Java players with Mojang. Disabling this explicitly permits offline identities.
online-mode = true
# Accept incoming transfer handshakes. Transfers still authenticate normally; cookies do not prove identity.
accept-transfers = false
# Include the player's public IP in the Mojang session check when enabled.
prevent-client-proxy-connections = false
# -1 disables client compression; 0 compresses every packet.
compression-threshold = 256

[runtime]
# Session owners and a separate bounded pool for DNS, authentication and other slow work.
io-loops = 2
workers = 4
max-sessions = 4096
max-pending-work = 256
# Global retained network-buffer budget and ceiling for each session.
max-buffered-bytes = 268435456
session-buffer-bytes = 16777216
# Pause reads above high water and resume below low water: low < high < session ceiling.
high-water-bytes = 4194304
low-water-bytes = 2097152
# Monotonic deadlines in milliseconds, bounded to 1..300000.
handshake-timeout-ms = 10000
login-timeout-ms = 30000
connect-timeout-ms = 10000
read-timeout-ms = 30000
write-timeout-ms = 10000

[admission]
# Token buckets: sustained attempts per second and burst capacity, globally and per origin IP.
global-rate = 200
global-burst = 400
origin-rate = 10
origin-burst = 20
# An origin table at capacity rejects new origins until idle entries expire.
max-origins = 65536
origin-expiry-ms = 60000

[forwarding]
# Identity this proxy signs into basalt-scheme backends, and the private Ed25519 key file.
# The key is generated on first use and its public half is printed for the backends' trusted-keys.
proxy-id = "proxy"
key-file = ".basalt/forwarding.key"

[translation]
# auto: capable Basalt Server translates; otherwise Proxy needs a verified route.
# proxy: force translation here. off: require compatible client/backend protocols.
mode = "auto"
)";
}

std::string_view defaultBackendsToml() noexcept {
    return R"(# Ordered routing targets. Names must refer to entries in [backends].
[routing]
default = ["lobby"]

# Optional virtual-host routes, for example: "play.example.org" = ["lobby"]
[forced-hosts]

[backends.lobby]
host = "127.0.0.1"
port = 25566
# Backend Java protocol; compatibility is checked for the client/backend pair.
protocol = 777
# none, basalt, velocity, bungeecord, bungeeguard or safenet, explicitly chosen for this backend.
# none is viable for an offline vanilla backend; it does not forward premium UUID/properties.
# The backend must use a matching scheme. No fallback to weaker identity on security failure.
forwarding = "none"
# Allow GetServer, GetServers, UUID and player-local Connect on bungeecord:main from this backend.
# Client-originated control messages are always discarded. Other subcommands are unsupported.
bungee-messages = false
# Shared secret for velocity or token for bungeeguard/safenet. Keep this file private.
secret = ""
)";
}

} // namespace basalt::proxy::config
