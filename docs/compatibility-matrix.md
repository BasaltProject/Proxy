# Compatibility matrix

What has been exercised, and how. Last updated 2026-10-16. Everything here ran on Linux, with a bot as the client (Basalt Swarm),
never with the official client unless a row says so, and against real servers whose jars were checked against the SHA-256 the
vendor publishes. Nothing here is a promise about versions not listed.

## Routes

A session has one client version and, for each backend it reaches, one backend version. The proxy refuses any pairing it has no
route for before it enters a state it cannot leave.

| Client | Backend | Route | How it works |
|---|---|---|---|
| 26.3 | 26.3 (Basalt Server, Paper) | native | relayed as is |
| 1.8 to 26.2 (see below) | 26.3 | translated | the Wire chain rewrites packets between versions |
| N | N, for the versions below | same version | relayed untranslated; only packet ids are read through the version's tables |
| 26.3 | anything older | refused | no translation upward exists |
| N | M, N different from M, M not 26.3 | refused | |

A network may mix both kinds of backend for clients from 1.20.2 on: a 26.2 client moves between a Basalt Server 26.3 (translated) and a Paper 26.2
(same version) without reconnecting, checked over four moves in both directions. Before 1.20.2 a session uses one kind only.

### Same version, client and backend

Both on the same protocol, no translation. Forwarding checked against Paper in each mode that applies:

| Versions (protocol) | none | BungeeCord | Velocity modern |
|---|---|---|---|
| 1.8.8 (47), 1.9 (107) to 1.9.4 (110), 1.10.2 (210), 1.11 (315), 1.11.2 (316), 1.12 (335), 1.12.1 (338), 1.12.2 (340) | yes | 1.8.8, 1.12.2 | n/a before 1.13 |
| 1.13 to 1.15.2 (393 to 578, every one of them) | yes | 1.14.4 | 1.14.4 |
| 1.16 (735) to 1.18.2 (758, every one of them) | yes | 1.16.5, 1.18.2 | 1.16.5, 1.18.2 |
| 1.19 to 1.20.2 (759 to 764) | yes | 1.20.1 | 1.20.1 |
| 1.20.3 and 1.20.4 (765) to 26.2 (776) | yes | 1.21.4 | 26.2 and 1.21.4 |

Moving a player between two backends of the same version without reconnecting: 1.8.8, 1.9.4, 1.12.2, 1.13.2, 1.14.4, 1.16.5, 1.18.2,
1.19.4, 1.20.1, and every version from 1.20.3 to 26.2. Before 1.20.2 the client receives the new backend's join packet; its tab list and
boss bars from the previous backend are not cleared.

Versions Paper never published or no longer downloads (1.9, 1.9.1, 1.9.2, 1.11, 1.14, 1.16 and the Paperclip fetch of 1.12 and 1.12.1) were checked
against the Mojang server jar of that version, offline, forwarding none. A negative control (backend demanding forwarding, proxy sending none) fails as it should for BungeeCord and Velocity.

### Translated, client older than the backend

A bot of each of these versions logs in and plays against a Basalt Server 26.3 through the proxy (Velocity forwarding):
1.8, 1.9, 1.9.1, 1.9.2, 1.9.4, 1.10.2, 1.11, 1.11.2, 1.12, 1.12.1, 1.12.2, 1.13, 1.13.1, 1.13.2, 1.14, 1.14.1, 1.14.2, 1.14.3, 1.14.4, 1.15, 1.15.1, 1.15.2, 1.16, 1.16.1, 1.16.2,
1.16.3, 1.16.5, 1.17, 1.17.1, 1.18.1, 1.18.2, 1.19, 1.19.2, 1.19.3, 1.19.4, 1.20.1, 1.20.2, 1.20.4, 1.20.6, 1.21, 1.21.3, 1.21.4, 1.21.5, 1.21.6, 1.21.8, 1.21.10,
1.21.11, 26.1, 26.2. Versions that share a protocol (1.16.4 and 1.16.5, 1.20 and 1.20.1, and so on) count as one. The login packets of each version are checked
for their exact shape. Reconfiguration without reconnecting is checked for 1.20.2 and later.

## Modded backends

| Backend | Result |
|---|---|
| Fabric loader 0.20.0 on 1.21.4, no mods, forwarding none | yes, transport gate passes |
| Fabric loader with Fabric API 0.119.4, forwarding none | yes, transport gate passes (Fabric API holds the join until the client answers a configuration ping) |
| The same plus FabricProxy-Lite 2.10.1 (Velocity modern forwarding) | login, configuration and the start of play work, FabricProxy-Lite asks for the player info twice and the proxy answers both as Velocity does; the bot receives no chunk in 30 seconds, cause not found, so not verified |
| Forge, NeoForge, modpacks | not tried |

A relay of the configuration state is not a promise about any particular modpack.

## Identity

| Mode | State |
|---|---|
| Offline | all of the above |
| Online (Mojang), 26.3 | login, session server lookup and forwarding of the verified profile checked with a simulated session server and a real Mojang endpoint; never with a paid account |
| Online, clients before 1.20.2 | encryption packets are written and read in each version's shape, not exercised end to end |
| Online, 1.19 to 1.19.2 clients that sign the encryption response | refused |

## Forwarding

| Mode | Backends |
|---|---|
| none | any of the above |
| Velocity modern | 1.13 and later |
| BungeeCord legacy | every version, plus BungeeGuard and a SafeNet-style token on 26.3 |
| Basalt (Ed25519 challenge) | Basalt Server 26.3 |

## Who translates: `protocol = "auto"`

A backend with `forwarding = "basalt"` and `protocol = "auto"` takes each client in the client's own version and the Basalt Server translates; the
proxy keeps translating clients before 1.13, which cannot carry the signed login query. Checked with a Basalt Server 26.3 in basalt forwarding:
1.8 and 1.12.2 (translated by the proxy), 1.16.5 and 1.21.4 (translated by the Server). Any other backend gets the newest protocol and the proxy
translates, as before. `[translation] mode = "proxy"` forces the proxy for all of them.

## Bedrock to a Basalt Server 26.3

Checked with the Swarm Bedrock bot against a Basalt Server 26.3 on a void world, Velocity forwarding. The bot is not the game client.

| What | Checked |
|---|---|
| Login, start game, chunks with biomes, spawn | yes |
| Position and movement to the backend | yes |
| Chat and commands to the backend, messages back | yes (the backend answers a late command only once its own tick has room) |
| Messages translated from a language file (`bedrock.language`) | yes, with the Mojang server jar of the operator |
| Other players: list, appear, move, leave | yes |
| Mobs appear with their Bedrock name and their hit box | yes (the bot does not report the box) |
| Inventory contents, after a `give` | yes |
| Time of day and rain | yes |
| Plugin events, `player:send` and `player:kick` for a Bedrock player | yes |
| Block changes, break and place | sent, not seen by the bot |
| Dropped items | yes (counts and item ids seen) |
| Moving a Bedrock player to another backend (`player:connect`): the old world is cleared, the new one streams in, spawn again | yes, between two Basalt Servers |
| 150 Bedrock bots at once, release build | yes, all enter; 226 of 400 enter on this machine, where the bots and the backend use more CPU than the proxy (73% of one core) |
| Bedrock connections go through the same admission gate as Java ones (global and per-origin token buckets) | yes: 32 of 40 bots arriving at once from one address enter with the defaults |
| A RakNet peer that leaves splits open, holds ordered frames back or never acknowledges is cut off | yes (16 splits, 4 MiB of parts, 256 held frames, 8192 unacknowledged datagrams) |
| Titles, subtitles, the action bar | yes |
| Boss bars (create, rename, progress) | yes |
| The sidebar scoreboard (objective, title, lines) | yes |
| Open containers, mob names and variants | not done |

## Under stress

Under the ASan proxy with 100 Java bots (1.21.4, translated) on a Basalt Server, killing the backend halfway left the proxy serving, and a
termination signal ended it cleanly with no sanitizer report. Fuzzing of RakNet sessions, the envelope and the Bedrock packet readers (about
60,000 sessions and 200,000 inputs per seed) found nothing under ASan and UBSan.

## Limits that matter

- A client newer than its backend is not supported.
- Bedrock clients (protocol 2193) join the first backend of the default route as a Java client. See the Bedrock section.
- The official game client has not been tried against this proxy; every client above is a bot.
- Windows and macOS builds compile and the Windows one ran under wine; the macOS backend of the event loop ran on FreeBSD.
