<p align="center">
  <img src="docs/assets/basalt-logo.png" alt="Basalt" width="360">
</p>

<h2 align="center">Basalt Proxy</h2>

<p align="center">
  <strong>A Minecraft proxy written from scratch in C++23. No external JVM.</strong>
</p>

<p align="center">
  <img alt="Minecraft" src="https://img.shields.io/badge/Minecraft-26.3-62B47A?logo=minecraft&logoColor=white">
  <img alt="Protocol" src="https://img.shields.io/badge/protocol-777-62B47A">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white">
  <img alt="Platforms" src="https://img.shields.io/badge/platforms-macOS_%7C_Linux_%7C_Windows-00599C">
  <img alt="Build" src="https://img.shields.io/badge/build-CMake-064F8C?logo=cmake&logoColor=white">
  <img alt="Plugins" src="https://img.shields.io/badge/plugins-Lua-000080?logo=lua&logoColor=white">
  <img alt="License" src="https://img.shields.io/badge/license-GPL--3.0-green">
</p>

> ⚠️ **Early development.** Basalt Proxy is not ready for any network, public or
> private. The current listener serves status requests; native login and relay are
> being tested with synthetic peers.
> The status table below is
> the honest state of the project.

## Overview

Basalt Proxy is a Minecraft proxy project written in **C++23**, distributed as a
native binary and sharing [Wire](https://github.com/BasaltProject/Wire) with
[Basalt Server](https://github.com/BasaltProject/Server).

The current executable provides `--help`, `--version` and `--config`. On startup,
it generates commented `proxy.toml` and `backends.toml` files when missing, loads
them and validates resource limits, backend identities and routing targets.
Existing files are preserved; invalid configuration reports its file and exits
with code 1. The listener handles handshake, local status and ping/pong on bounded
nonblocking I/O owners. Admission limits apply globally and per origin; incomplete
handshakes expire. SIGINT/SIGTERM stop the runtime and release sessions. Player
login performs the RSA/AES exchange and bounded Mojang session lookup on workers.
Offline mode is explicit. The first verified session reserves its UUID until close; duplicate
logins are rejected before backend resolution, and can reconnect after the first session closes.
Accepted identities select their configured route,
resolve destinations on bounded workers and establish a nonblocking TCP connection,
trying the next destination after DNS or connection failure, including connection-stage
deadlines within the absolute login deadline. Late canceled DNS results are discarded.
For native 26.3 backends
with explicit `none`, `velocity`, `bungeecord`, `bungeeguard` or `safenet` forwarding,
it sends the login handshake and identity and relays
login disconnects, decoding backend compression independently of frontend framing.
Velocity modern queries receive signed v1/v4 identity responses; missing or duplicate forwarding
queries are rejected. Offline bot gates against Paper and Basalt pass; real online-profile
interoperability remains pending.
Legacy handshakes carry the verified UUID/IP and configured token, with a bounded 32,767-byte
host. Credential properties echoed by a backend are removed before frontend login success.
These handshakes require a private backend link; legacy forwarding does not encrypt credentials.
Unknown login queries receive a matching negative response using backend compression.
A backend may enable `bungee-messages = true` in its `backends.toml` table to use
`GetServer`, `GetServers` and `UUID` on `bungeecord:main`. The default is false.
The legacy `BungeeCord` alias is recognized and replies use the native channel.
Client control messages are discarded. Other subcommands, including administrative actions,
are currently ignored. Responses use bounded session buffers and the verified player UUID.
Login cookie requests are tracked by their backend and relayed to the client when uncached;
matching responses are bounded to 5,120 bytes.
Configuration/play cookie requests, responses and stores are validated before relay, with up to
32 pending requests and 256-byte keys. Unsolicited or mismatched responses close the session.
A session-local cookie cache retains up to 32 entries and 64 KiB of encoded content under
buffer quotas. Known login cookie queries are answered from it; an older client response
cannot overwrite a newer backend Store. Unknown, absent and empty cookies remain distinct.
Incoming transfer handshakes require `network.accept-transfers = true` and still authenticate
normally. Backend transfers validate the destination and drain before closing the old connection;
they reconnect the client and do not perform an internal backend switch.
Native LoginFinished/ACK and configuration/play transitions relay small packets in
synthetic tests, retaining the verified frontend UUID. Reconfiguration waits for the client
ACK before decoding buffered configuration traffic; repeated cycles have synthetic coverage.
The latest client settings from configuration or play are retained under session quotas
and replayed in configuration form after the reconfiguration ACK.
The login timeout bounds initial setup through the first configuration acknowledgement;
backend traffic and read suspension do not extend that absolute budget.
Configuration/play buffers grow
within session/global quotas; bidirectional 512 KiB payload fixtures pass. Backend login
packets are bounded to 64 KiB and profiles to 64 properties. Frontend compression is
negotiated before login success and remains
independent of backend compression. Directional high/low watermarks pause producers
while the receiving endpoint drains. An independent Swarm bot has reached play through the proxy
on real offline vanilla, Paper and Basalt 26.3 backends; a frontend recorder verified registry/chunk traffic,
UUID, teleport confirmation and keepalive round trips. Paper modern forwarding also rejected
a mismatched secret and absent forwarding, as did Basalt. Graphical clients and online profiles
remain unvalidated.

## Product goals

The intended proxy will route players between Minecraft backends, including
servers other than Basalt, and support Java and Bedrock through Wire. Version
translation will be configurable and delegated to a capable Basalt backend when
negotiated. Routing, forwarding, translation and plugins are under development.

Native plugins will use Lua and `.bppkg` packages, following the server's API
conventions for shared concepts. Future Velocity and BungeeCord adapters will
belong to Proxy, after [Shim](https://github.com/BasaltProject/Shim) is complete.
Shim provides Java bytecode execution through a JVM implemented in C++; the
adapters provide the plugin APIs. No external JVM installation is required.

## Building

The network layer lives in [Wire](https://github.com/BasaltProject/Wire), shared
with the server and pulled in as a submodule, so clone with it:

```bash
git clone --recurse-submodules https://github.com/BasaltProject/Proxy.git
```

In an existing clone, `git submodule update --init` does the same thing. Without
it the build fails immediately: most of the modules are missing.

You need CMake 3.28 or newer, Ninja and Clang. Then two commands, the same on
macOS, Linux and Windows:

```bash
cmake --preset release
cmake --build build/release
```

That leaves the binary in `build/release/bin/`, named for what it is:

```
build/release/bin/BasaltProxy-26.3.0-dev-macOS-arm64
build/release/bin/BasaltProxy-26.3.14-Linux-x64
build/release/bin/BasaltProxy-26.3.14-Windows-x64.exe
```

The version is `<minecraft>.<build>`: which Minecraft the proxy speaks natively,
and how far along that line the build is. A build made outside the CI has no
build number, so it is `.0-dev`.

## Testing

```bash
# Everything CI runs
cmake --preset debug && cmake --build build/debug
ctest --preset debug

# A single suite
./build/debug/proxy-runtime/proxy-runtime-cli-test
```

`--preset asan` builds the same tree under the address and undefined sanitizers.

An optional independent bot gate uses an already running offline vanilla 26.3 server
on loopback and a separately built Swarm binary:

```bash
python3 tools/verify-native-backend.py \
  --proxy build/debug/bin/BasaltProxy-26.3.0-dev-Linux-x64 \
  --swarm ../Swarm/build/debug/bin/BasaltSwarm-26.3.0-dev-Linux-x64 \
  --backend-port 25566 --output build/native-gate
```

It creates an isolated proxy configuration, records frontend frames independently of Wire,
and writes `result.json` and process logs. It requires a 22-second session, verified offline
UUID, registries, play/chunk packets and matching teleport/keepalive responses. It does not
validate chunk semantics, graphical clients or online authentication.

For a Paper backend configured for Velocity modern forwarding and offline proxy identities,
add `--forwarding velocity --secret-file <file>` with its matching secret. The generated backend
configuration is private; reports contain no secret. `--expect-login-rejection` instead requires
a login disconnect with no accepted profile or play traffic, for negative forwarding checks.
Swarm diagnostics remain visible in `bot_diagnostics`; Paper's `paper:raw` registry entry
currently produces one. This transport gate does not certify registry semantics.


## Requirements

| Component | Requirement |
|---|---|
| CMake | 3.28 or newer |
| Generator | Ninja |
| Compiler | Clang, with C++23 support |
| ccache | Optional, but the build is much faster with it |

## Development Status

| Area | Status |
|---|---|
| Build, module layout and cross-platform CI | Configured; platform validation pending |
| Command line: `--help`, `--version`, `--config` | Working |
| Network layer, shared with the server | Inherited from Wire |
| Listener, handshake, local status and ping/pong | Implemented, tested on Linux |
| Initial login crypto and session verification | Implemented; simulated-endpoint tests |
| Native backend login, configuration and play relay | Offline vanilla/Paper/Basalt 26.3 bot gates pass; graphical client pending |
| Backend connections and server switching | TCP connection implemented; switching pending |
| Player forwarding to the backend | Velocity modern tested with offline Paper/Basalt 26.3; legacy/token modes have fixture coverage |
| Configuration files | Generated, loaded and validated at startup |
| Translation handover to a Basalt Server backend | Not started |
| Older Java clients and Bedrock clients | Not started |
| Plugins and the Lua runtime | Not started |
| Velocity and BungeeCord adapters using Shim | Deferred until Shim is complete |

Tests use synthetic clients and backends on Linux. No full registry/chunk sequence
or real game client has been validated.

## Related repositories

| Repository | What it is |
|---|---|
| [Server](https://github.com/BasaltProject/Server) | The Minecraft server itself, in C++23 |
| [Wire](https://github.com/BasaltProject/Wire) | The network layer both this and the server build on |
| [Website](https://github.com/BasaltProject/Website) | The project site, at [basaltmc.org](https://basaltmc.org) |

## License

Basalt Proxy is distributed under the [GNU General Public License v3.0](LICENSE).

Basalt is not affiliated with Mojang Studios or Microsoft. Minecraft is a
trademark of Mojang Studios.
