<p align="center">
  <a href="https://raw.githubusercontent.com/BasaltProject/.github/main/assets/basalt-logo-white.svg#gh-dark-mode-only">
    <img src="https://raw.githubusercontent.com/BasaltProject/.github/main/assets/basalt-logo-white.svg" alt="Basalt" width="360">
  </a>
  <a href="https://raw.githubusercontent.com/BasaltProject/.github/main/assets/basalt-logo-black.svg#gh-light-mode-only">
    <img src="https://raw.githubusercontent.com/BasaltProject/.github/main/assets/basalt-logo-black.svg" alt="Basalt" width="360">
  </a>
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
> private. It has only been exercised on Linux with offline bots and simulated
> backends. The status table below is the honest state of the project.

## Overview

Basalt Proxy sits in front of a set of Minecraft servers, gives players a single
address and moves them between backends. It is written entirely in **C++23**,
shipped as a native binary, and shares the network layer with
[Basalt Server](https://github.com/BasaltProject/Server) through
[Wire](https://github.com/BasaltProject/Wire).

The native core requires no external JVM. Future Java plugin compatibility will
use [Shim](https://github.com/BasaltProject/Shim), a JVM implemented in C++, once
that project is complete.

Two goals drive every decision:

- **Any backend.** The proxy speaks the standard protocol with whatever is behind
  it: vanilla, Paper, Spigot, Fabric or Basalt. A Basalt Server behind it is the
  best case, never a requirement.
- **A plugin platform worth switching to.** Native plugins are written in Lua and
  packaged as `.bppkg`, following the server's API conventions for shared concepts.
  Velocity and BungeeCord adapters will come later, on top of Shim.

| | |
|---|---|
| Language | C++23 native core; no external JVM required |
| Target version | Minecraft 26.3 (protocol 777) |
| Distribution | Native binaries per platform |
| Backends | Any Minecraft server; 26.3 natively, and older ones from 1.21.5 to 26.2 for 26.3 clients |
| Plugin API | Lua, `.bppkg` packages (working; see `examples/proxy-plugin`) |
| Configuration | A `proxy.toml` and a `backends.toml`, generated with comments |
| License | GPL-3.0 |

## Configuration

On first start the proxy writes commented defaults into `./config/`, or into the
directory given with `--config <dir>`:

```
config/
  proxy.toml      network, runtime limits, admission, translation
  backends.toml   routing, forced hosts, and one table per backend
```

Existing files are never overwritten. A malformed file is reported by name and the
proxy exits with code 1. A backend looks like this:

```toml
[routing]
default = ["lobby"]

[backends.lobby]
host = "127.0.0.1"
port = 25566
protocol = 777
forwarding = "velocity"
secret = "change-me"
bungee-messages = false
```

Keep `backends.toml` private: it holds forwarding secrets.

## Console

Commands are `help`, `servers`, `status`, `send <player> <backend>` and `stop`.
Player names match the verified name exactly, including case. A successful `send`
acknowledges preparation, and the destination may still reject the login. It uses
the same change a Bungee `Connect` does and needs no plugins. Console commands do
not intercept player chat or touch the client's command tree.

Input accepts an optional leading slash, CRLF and a final line at EOF. EOF leaves
the proxy running, for headless services. Lines longer than 4096 bytes are
discarded through their delimiter.

## Building

The network layer lives in [Wire](https://github.com/BasaltProject/Wire), shared
with the server and pulled in as a submodule, so clone with it:

```bash
git clone --recurse-submodules https://github.com/BasaltProject/Proxy.git
```

In an existing clone, `git submodule update --init` does the same. Without it the
build fails immediately: most of the modules are missing.

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

The version is `<minecraft>.<build>`. A build made outside the CI has no build
number, so it is `.0-dev`. `ccache` is optional, but the build is much faster
with it.

The tests run with `ctest --preset debug`, and `--preset asan` builds the same
tree under the address and undefined sanitizers.

## Status

| Area | Status |
|---|---|
| Build, module layout and cross-platform CI | Configured; platform validation pending |
| Command line: `--help`, `--version`, `--config` | Working |
| Listener, handshake, local status and ping/pong | Implemented, tested on Linux |
| Login crypto and session verification | Implemented; simulated-endpoint tests |
| Native backend login, configuration and play relay | Offline vanilla, Paper and Basalt 26.3 bot gates pass; graphical client pending |
| Server switching | Verified with the Swarm bot between real Paper and Basalt backends in one connection; graphical client pending |
| Player forwarding | Velocity modern tested with offline Paper and Basalt; legacy modes have fixture coverage |
| Online profiles and graphical clients | Not validated |
| Translation handover to a Basalt Server backend | Working for Java and Bedrock players over the session envelope (bot-verified) |
| Older Java clients, 26.3 clients on older servers, and Bedrock clients | Older clients from 1.8 and 26.3 clients on 1.21.5 to 26.2 bot-verified; Bedrock bridge bot-verified; no graphical client yet. See `docs/compatibility-matrix.md` |
| Plugins and the Lua runtime | Working, sandboxed, hot reload; Linux only so far |
| Velocity and BungeeCord adapters using Shim | Deferred until Shim is complete |

## Related repositories

| Repository | What it is |
|---|---|
| [Server](https://github.com/BasaltProject/Server) | The Minecraft server itself, in C++23 |
| [Wire](https://github.com/BasaltProject/Wire) | The network layer both this and the server build on |
| [Shim](https://github.com/BasaltProject/Shim) | A JVM implemented in C++, for future Java plugin compatibility |
| [Website](https://github.com/BasaltProject/Website) | The project site, at [basaltmc.org](https://basaltmc.org) |

## License

Basalt Proxy is distributed under the [GNU General Public License v3.0](LICENSE).

Basalt is not affiliated with Mojang Studios or Microsoft. Minecraft is a
trademark of Mojang Studios.
