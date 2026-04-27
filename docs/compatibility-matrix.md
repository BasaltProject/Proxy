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
| 1.8.8 (47), 1.9.4 (110), 1.10.2 (210), 1.11.2 (316), 1.12 (335), 1.12.1 (338), 1.12.2 (340) | yes | 1.8.8, 1.12.2 | n/a before 1.13 |
| 1.13 to 1.15.2 (393 to 578, including 1.14.2 and 1.15.1) | yes | 1.14.4 | 1.14.4 |
| 1.16.1 (736) and 1.16.2 to 1.18.2 (751 to 758, including 1.17 and 1.17.1) | yes | 1.16.5, 1.18.2 | 1.16.5, 1.18.2 |
| 1.19 to 1.20.2 (759 to 764) | yes | 1.20.1 | 1.20.1 |
| 1.20.3 and 1.20.4 (765) to 26.2 (776) | yes | 1.21.4 | 26.2 and 1.21.4 |

Moving a player between two backends of the same version without reconnecting: 1.8.8, 1.9.4, 1.12.2, 1.13.2, 1.14.4, 1.16.5, 1.18.2,
1.19.4, 1.20.1, and every version from 1.20.3 to 26.2. Before 1.20.2 the client receives the new backend's join packet; its tab list and
boss bars from the previous backend are not cleared.

Not covered: 1.16 (735, Paper never published it). The 1.12 jars come from Mojang by hand because Paperclip's own download URL is gone. A negative control (backend demanding forwarding, proxy sending none) fails as it should for BungeeCord and Velocity.

### Translated, client older than the backend

A bot of each of these versions logs in and plays against a Basalt Server 26.3 through the proxy (Velocity forwarding):
1.8, 1.9, 1.9.2, 1.11, 1.12, 1.12.1, 1.12.2, 1.13, 1.13.1, 1.13.2, 1.14, 1.14.1, 1.14.3, 1.14.4, 1.15, 1.15.1, 1.15.2, 1.16, 1.16.1, 1.16.2,
1.16.3, 1.17, 1.17.1, 1.18.2, 1.19, 1.19.3, 1.19.4, 1.21, 1.21.4, 1.21.5, 1.21.6, 1.21.11, 26.1, 26.2. The login packets of each version are checked
for their exact shape. Reconfiguration without reconnecting is checked for 1.20.2 and later.

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

## Limits that matter

- A client newer than its backend is not supported.
- Bedrock clients get a status listing and are turned away after login.
- The official game client has not been tried against this proxy; every client above is a bot.
- Windows and macOS builds compile and the Windows one ran under wine; the macOS backend of the event loop ran on FreeBSD.
