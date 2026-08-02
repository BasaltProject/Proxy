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

> ⚠️ **Early development.** Basalt Proxy is not ready for a public network. It has
> only been exercised on Linux, mostly with offline bots against real vanilla, Paper
> and Basalt backends. The status table below is the honest state of the project.

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

Commands run from the console and from players (Java and Bedrock), with a command tree,
permissions and tab-complete: `help`, `list`, `glist`, `server`, `send`, `status`,
`reload`, `plugins`, `op`, `deop`, `group`, `ban`, `pardon`, `banlist`, `kick`,
`whitelist`, `antibot` and `stop`. Who may run what is defined in `groups.toml`
(inheritance, `*` wildcards, a `-` prefix to deny; the most specific rule wins); who
belongs to which group is player state, set with `op` and `group`. Everything is also
reachable as `basalt:<name>`, which a backend can never shadow. Whitelist and bans are
applied at login for Java and Bedrock players. A successful `send` acknowledges
preparation, and the destination may still reject the login.

The console accepts an optional leading slash, CRLF and a final line at EOF. EOF leaves
the proxy running, for headless services.

## Antibot

`antibot.toml` configures verification that runs before any server is chosen. It scores
each origin address (reputation, name and host patterns, request rate), switches to
attack mode on its own when the load spikes, and can hold suspicious logins in a small
native limbo world where gravity, movement, keep-alive latency and client brand are
checked. A block or map captcha is available, on by default only for very suspicious
origins. Also included: an optional passthrough that sends new origins away once and
admits them when they return, an exploit filter (Log4j-style strings, oversized plugin
messages, NBT limits), a status cache, blocklists from files or HTTPS URLs, optional
anti-VPN and GeoIP lookups using your own provider and database, Linux `TCP_INFO`
checks, a hook that runs a command (for example an nftables update) to drop banned
addresses in the kernel, attack summaries, a webhook, and the `antibot` console command.
The Bedrock listener shares reputation and bans. Verified with Swarm and raw-socket
bots against real Paper and Basalt backends; it has not been run with the official
graphical client, and the map captcha has not been solved end to end.

## Lua plugins

Plugins are `.bppkg` packages with a `plugin.toml` and a Lua main script, running in a
sandbox with an instruction and memory budget; a broken plugin does not take the proxy
down. The API shares the shape of the server's: `on` and `watch` events, commands,
players and servers, a store, waits, and a `packet` namespace to read, modify, cancel
and send packets in both directions. Some events (login, server pre-connect, kick, chat,
command, server list ping) accept a decision within a deadline that never blocks the
network loops. `plugins.toml` grants each plugin its capabilities. Off by default.

## BungeeCord and Velocity plugins

An optional layer runs BungeeCord and Velocity plugins on top of Shim, without a JVM
installed on the host. Without Java plugins in `plugins/`, no virtual machine starts.
The adapters map to the native API, covering the `BungeeCord` plugin channel and the
common events; the BungeeCord adapter is the more complete one and Velocity still throws
`UnsupportedOperationException` for titles, boss bars, tab lists, cookies and transfers.
Only test plugins have been exercised; no community plugin has been validated.

## Remote API

`remote.toml` enables a small HTTP API (off by default, loopback) to read the proxy's
state (`/v1/status`, `/v1/servers`, `/v1/players`, `/v1/sessions`...) and to send or kick
a player and reload the configuration. It uses a bearer token generated on first start
in `.basalt/api.token`, locks out addresses that send wrong tokens, and is plain HTTP, so
anything beyond loopback belongs behind a TLS reverse proxy. Each action can be turned off.

## Bedrock

With `[bedrock] enabled = true` the proxy listens for Bedrock Edition clients over
NetherNet and RakNet and bridges them to a Java server, translating chunks, position and
chat. A Basalt Server can serve Bedrock players itself over the session envelope. Verified
with a bot; NetherNet has only been checked for its status response, and no real Bedrock
client has joined yet.

## Server list

`server_list.toml` defines what the multiplayer list shows: MOTD, player count and sample,
a default or custom icon and named profiles with conditions, optionally relaying the
status of a real server. It supports `%basalt_*%` placeholders and MiniMessage.

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
| Build, module layout and cross-platform CI | CI builds Linux x64/arm64, macOS x64/arm64 and Windows x64; only Linux has been run |
| Command line: `--help`, `--version`, `--config` | Working |
| Listener, handshake, local status and ping/pong | Implemented, tested on Linux |
| Login crypto and session verification | Implemented; simulated-endpoint tests and a real Mojang TLS query; no real account yet |
| Native backend login, configuration and play relay | Offline vanilla, Paper and Basalt 26.3 bot gates pass; graphical client pending |
| Server switching | Verified with the Swarm bot between real Paper and Basalt backends in one connection; graphical client verified offline |
| Player forwarding | Velocity modern tested with offline Paper and Basalt; legacy modes have fixture coverage |
| Configuration, groups, commands, whitelist and bans | Working; verified with Paper and bots |
| Antibot | Working; verified with bots, not with a graphical client |
| Online profiles and graphical clients | Not validated |
| Translation handover to a Basalt Server backend | Working for Java and Bedrock players over the session envelope (bot-verified) |
| Older Java clients, 26.3 clients on older servers, and Bedrock clients | Older clients from 1.8 and 26.3 clients on 1.21.5 to 26.2 bot-verified; Bedrock bridge bot-verified; no real Bedrock client yet |
| Plugins and the Lua runtime | Working, sandboxed, hot reload; Linux only so far |
| Remote HTTP API | Working; verified with `curl` and tests |
| Velocity and BungeeCord adapters using Shim | Early; test plugins only, Velocity has gaps |

## Contributing and community

Please read the [contribution guide](CONTRIBUTING.md) and
[Code of Conduct](.github/CODE_OF_CONDUCT.md) before participating.
Report suspected vulnerabilities privately using the [security policy](SECURITY.md).

## License

Basalt Proxy is distributed under the [GNU General Public License v3.0](LICENSE).

Basalt is not affiliated with Mojang Studios or Microsoft. Minecraft is a
trademark of Mojang Studios.
