<p align="center">
  <img src="docs/assets/basalt-logo.png" alt="Basalt" width="360">
</p>

<h2 align="center">Basalt Proxy</h2>

<p align="center">
  <strong>A Minecraft proxy written from scratch in C++23. No JVM.</strong>
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
> private. Today the binary starts, prints and exits. The status table below is
> the honest state of the project.

## Overview

Basalt Proxy sits in front of a set of Minecraft servers and gives players one
address to connect to, moving them between backends without a reconnect.

It is written entirely in **C++23** and ships as a native binary per
platform. There is no JVM anywhere in the runtime: no bundled JRE, no embedded
VM, no hidden Java process.

It is the proxy component of the **Basalt** project, and it shares its entire
network layer with [Basalt Server](https://github.com/BasaltProject/Server)
through [Wire](https://github.com/BasaltProject/Wire). One implementation of the
protocol, used by both sides.

| | |
|---|---|
| Language | C++23, 100 %. No Java, no JVM at runtime |
| Target version | Minecraft 26.3 (protocol 777) |
| Distribution | Native binaries per platform |
| Backends | Any Minecraft server, Basalt or not |
| Plugin API | Lua, `.bppkg` packages. Velocity and BungeeCord plugins optionally, see below |
| Message model | Adventure-style immutable components; no legacy `§` formatting |
| License | GPL-3.0 |

## It is not only for Basalt Server

A proxy that only works with its own server is a proxy nobody can adopt. Basalt
Proxy speaks the standard protocol to whatever is behind it, so vanilla, Paper,
Spigot, Fabric and anything else that accepts a Minecraft connection all work.

Basalt Server behind it is the better case, not the required one:

| Backend | What you get |
|---|---|
| Any Minecraft server | Routing, one address, moving players between backends, plugins, the whole proxy |
| Basalt Server | The above, plus work the two can hand to each other rather than each doing it alone |

## Where translation happens

Older Java clients and Bedrock clients are both supported by translating at the
edge, and both the proxy and the server carry that machinery. That is deliberate,
not duplicated effort: **the same translation can be done in either place, and
which one should do it depends on what is behind the proxy.**

The proxy is always the one that opens the port and accepts the connection. It is
also the process with the most connections open at once, so translating every one
of them there is the most expensive place to do it.

So the default is to push that work down when it can be pushed:

| Situation | Who translates | Why |
|---|---|---|
| Backend is Basalt Server | The server | The proxy is the connection-heavy process. The two speak to each other well enough to hand the work over |
| Backend is anything else | The proxy | A vanilla or Paper backend only understands its own version, so the proxy has to do it |

Both halves are configurable. An operator who wants every connection translated
at the proxy, or who wants it never translated at all, says so in the
configuration and the proxy does that instead. The table above is the default,
not a constraint.

## Plugins

### Basalt plugins

The proxy has its own plugin API. Plugins are written in Lua and packaged as
`.bppkg` files, the same shape as the server's `.bspkg`, and run in a sandbox
with a per-tick instruction budget, so a broken plugin cannot take the proxy
down.

It is a separate format from the server's on purpose: a proxy plugin and a server
plugin see different things, and pretending one API covers both would make the
worse version of each.

### Velocity and BungeeCord plugins

The existing ecosystem is worth a great deal, and most of it is Velocity and
BungeeCord plugins that will never be rewritten. Basalt Proxy can run them, and
doing so is **opt-in**:

- The proxy itself never requires a JVM. Its core, its network layer and its own
  plugins are native, and a proxy started without this feature never looks for
  Java.
- Enabling it means the operator supplies a JVM and accepts what that costs:
  memory and startup time far above what the native proxy uses on its own.
- It is a trade the operator makes deliberately, in the configuration, in
  exchange for the plugins they already depend on.

## Configuration

Configuration lives in a `config/` directory, one TOML file per domain, generated
with comments and sensible defaults on first start.

Anything that changes how the proxy behaves by default, including who performs
version translation, is an explicit setting rather than something inferred and
left unexplained.

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
| Listener, handshake and player routing | Not started |
| Backend connections and server switching | Not started |
| Player forwarding to the backend | Not started |
| Configuration files | Not started |
| Translation handover to a Basalt Server backend | Not started |
| Older Java clients and Bedrock clients | Not started |
| Plugins and the Lua runtime | Not started |
| Velocity and BungeeCord plugin support | Not started |

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
