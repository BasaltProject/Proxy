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
> private. The current listener serves status requests and rejects player login.
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
login is explicitly rejected until authentication and backend routing are integrated.

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
| Build, module layout and cross-platform CI | Working |
| Command line: `--help`, `--version`, `--config` | Working |
| Network layer, shared with the server | Inherited from Wire |
| Listener, handshake, local status and ping/pong | Implemented, tested on Linux |
| Authentication and player routing | Not started |
| Backend connections and server switching | Not started |
| Player forwarding to the backend | Not started |
| Configuration files | Generated, loaded and validated at startup |
| Translation handover to a Basalt Server backend | Not started |
| Older Java clients and Bedrock clients | Not started |
| Plugins and the Lua runtime | Not started |
| Velocity and BungeeCord adapters using Shim | Deferred until Shim is complete |

What that adds up to today: a binary that parses its command line and exits.
Everything below that line is still missing.

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
