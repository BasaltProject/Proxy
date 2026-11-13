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

## Overview

Basalt Proxy sits in front of a set of Minecraft servers, gives players a single
address and moves them between servers. It is written entirely in **C++23**,
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
  Velocity and BungeeCord plugins can optionally run through adapters on top of Shim
  (early, see [Compatibility](#bungeecord-and-velocity-plugins)).

| | |
|---|---|
| Language | C++23 native core; no external JVM required |
| Target version | Minecraft 26.3 (protocol 777) |
| Distribution | Native binaries per platform |
| Backends | Any Minecraft server; 26.3 natively, and older ones from 1.21.5 to 26.2 for 26.3 clients |
| Plugin API | Lua, `.bppkg` packages (working); optional BungeeCord and Velocity adapters (early) |
| Configuration | One commented TOML file per domain in `config/`, generated on first start |
| License | GPL-3.0 |

## Download and documentation

Binaries for Linux, macOS and Windows are on the [releases page](https://github.com/BasaltProject/Proxy/releases):
a stable release every Monday when there is something new, and a moving **Nightly** prerelease that follows `main`.
Each asset is the executable itself, with a `SHA256SUMS` file beside them. The full documentation, including the
plugin API, is at [basaltmc.org/docs](https://basaltmc.org/docs/proxy/introduction).

## Configuration

On first start the proxy writes commented defaults into `./config/`, or into the
directory given with `--config <dir>`:

```
config/
  proxy.toml        network, runtime limits, admission, forwarding, logging, Bedrock, translation
  servers.toml      the servers, their order, forced hosts, server groups, health checks and status relay
  server_list.toml  the multiplayer-list entry: MOTD, player sample, icon, profiles
  limits.toml       packet rate and chat/command spam limits per connection
  groups.toml       permission groups
  commands.toml     command options and custom commands
  messages.toml     kick and command messages
  antibot.toml      bot protection
  remote.toml       the HTTP administration API
  plugins.toml      the Lua plugin runtime
  tab_list.toml  boss_bar.toml  scoreboard.toml  animations.toml
```

Existing files are never overwritten. A malformed file is reported by name and the
proxy exits with code 1. State written by the proxy (players, whitelist, bans, tokens,
logs) lives apart from the configuration, in `.basalt/`. Most files reload while the
proxy runs (`reload`, `SIGHUP` or the API); one that does not validate is reported and
the running settings stay. A server looks like this:

```toml
[routing]
try = ["lobby"]

[servers.lobby]
host = "127.0.0.1"
port = 25566
protocol = 777
forwarding = "velocity"
secret = "change-me"
bungee-messages = false
```

`forwarding` is chosen per server: `none`, `basalt` (signed Ed25519), `velocity`,
`bungeecord`, `bungeeguard` or `safenet`, with no silent fallback to a weaker one.
Keep `servers.toml` private: it holds forwarding secrets. Servers can be grouped,
routed by the address the player typed (`[forced-hosts]`), and health-checked in the
background.

Put the servers on a private network or behind a firewall that admits only the proxy.
`none`, `bungeecord`, `bungeeguard` and `safenet` carry no proof of origin, so they are
safe only on a port nobody else can reach.

Behind a load balancer that sends a PROXY protocol header (v1 or v2), set
`proxy-protocol = true` and list the balancer in `proxy-protocol-trusted`; the header is
only believed from those addresses, so a player cannot forge an address.

## Groups and commands

Built-in commands (`server`, `send`, `glist`, `kick`, `ban`, `whitelist`, `reload`...) work from the console and in game,
with tab-complete and a `basalt:` prefix backends cannot shadow. Permissions live in `groups.toml`, with inheritance,
wildcards and negation.

## Antibot

`antibot.toml` checks every connection before it reaches a server: per-address reputation, automatic attack mode, a
limbo world with movement and captcha checks, an exploit filter, blocklists and optional anti-VPN and GeoIP lookups.

## Lua plugins

Plugins are `.bppkg` packages written in Lua and run in a sandbox, so a broken plugin cannot take the proxy down. The
API follows the server's: events, commands, players, servers, a store and packet access. Off by default.

## BungeeCord and Velocity plugins

Optional adapters run BungeeCord and Velocity plugins on Shim, with no JVM installed on the host. No virtual machine
starts unless there are Java plugins in `plugins/`.

## Remote API

`remote.toml` enables an HTTP API for reading the proxy's state, moving and kicking players and reloading the
configuration. It is off by default, listens on loopback and uses the token in `.basalt/api.token`.

## Bedrock

With `[bedrock] enabled = true`, Bedrock Edition players join over NetherNet and are bridged to Java servers. A Basalt
Server behind the proxy can serve them directly.

## Server list

`server_list.toml` sets the MOTD, player count and sample, icon and conditional profiles, with `%basalt_*%`
placeholders and MiniMessage.

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

## Contributing and community

Please read the [contribution guide](CONTRIBUTING.md) and
[Code of Conduct](.github/CODE_OF_CONDUCT.md) before participating.
Report suspected vulnerabilities privately using the [security policy](SECURITY.md).

## License

Basalt Proxy is distributed under the [GNU General Public License v3.0](LICENSE).

Basalt is not affiliated with Mojang Studios or Microsoft. Minecraft is a
trademark of Mojang Studios.
