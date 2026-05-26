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
| 26.3 | 26.2 (`protocol = 776`), 26.1 (775), 1.21.11 (774), 1.21.9 and 1.21.10 (773) or 1.21.7 and 1.21.8 (772), 1.21.6 (771) or 1.21.5 (770) | carried upward | the Wire chain read upward: ids, login, chunks and light, particles, movement, advancements, command parsers, trim materials and the three registries 26.3 added |
| 26.3 | anything older than 1.21.5 | refused | no translation upward exists |
| N | M, N different from M, M not 26.3 | refused | |

A 26.3 client reached a real Paper 26.2, Paper 26.1.2, Paper 1.21.11, Paper 1.21.10, Paper 1.21.8, Paper 1.21.6 and Paper 1.21.5 through the proxy (three bots saw each other on the last) with the Swarm bot and the independent frame recorder (Paper 1.21.10 as well) (joined, 29 keep-alives answered, teleport confirmed,
the command tree identical to what a 26.2 bot sees directly, commands, summon and block use). It also moved four times between two Paper 26.2 and between a Paper 26.2 and a
Basalt Server 26.3 in one connection. The Swarm bot's own checks of what a server sends against 26.3 (registries and their elements, tags, command argument registries) report nothing on the four routes,
apart from the `paper:raw` chat type Paper adds itself. The brewing stand's item lists are added from a real 26.3 server's. The registries of 1.21.11 were rewritten into 26.1's and checked against what a real 26.1 server sends (two entries differ, both vanilla data changes: the lunge enchantment
and the End's sky light colour). Not converted: the item
components that changed shape (a trim with an inline material, pot decorations, teleport effects). No graphical client has been tried.

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
1.19.4, 1.20.1, and every version from 1.20.3 to 26.2. Before 1.20.2 the client receives the new backend's join packet; the proxy first removes the tab list entries and boss
bars it saw from the previous backend (the tab list removal was seen with a 1.16.5 client; boss bars were not observed with a real bar).

Versions Paper never published or no longer downloads (1.9, 1.9.1, 1.9.2, 1.11, 1.14, 1.16 and the Paperclip fetch of 1.12 and 1.12.1) were checked
against the Mojang server jar of that version, offline, forwarding none. A negative control (backend demanding forwarding, proxy sending none) fails as it should for BungeeCord and Velocity.

### Translated, client older than the backend

A bot of each of these versions logs in and plays against a Basalt Server 26.3 through the proxy (Velocity forwarding):
1.8, 1.9, 1.9.1, 1.9.2, 1.9.4, 1.10.2, 1.11, 1.11.2, 1.12, 1.12.1, 1.12.2, 1.13, 1.13.1, 1.13.2, 1.14, 1.14.1, 1.14.2, 1.14.3, 1.14.4, 1.15, 1.15.1, 1.15.2, 1.16, 1.16.1, 1.16.2,
1.16.3, 1.16.5, 1.17, 1.17.1, 1.18.1, 1.18.2, 1.19, 1.19.2, 1.19.3, 1.19.4, 1.20.1, 1.20.2, 1.20.4, 1.20.6, 1.21, 1.21.3, 1.21.4, 1.21.5, 1.21.6, 1.21.8, 1.21.10,
1.21.11, 26.1, 26.2. Versions that share a protocol (1.16.4 and 1.16.5, 1.20 and 1.20.1, and so on) count as one. The login packets of each version are checked
for their exact shape. Reconfiguration without reconnecting is checked for 1.20.2 and later.

## Registries and content a version cannot show

The proxy never substitutes content. A client gets the registries of the backend it is on, sent again at every backend change (from 1.20.2 the
client reconfigures; before that a translated client gets the new backend's join packet and the lists described in the plan are cleared). A
client and backend pair that has no verified route is refused with a message before any state is created: at the handshake for the first
backend, and as a refused `player:connect` for a change. Nothing is rewritten between registries of different versions.

## Modded backends

| Backend | Result |
|---|---|
| Fabric loader 0.20.0 on 1.21.4, no mods, forwarding none | yes, transport gate passes |
| Fabric loader with Fabric API 0.119.4, forwarding none | yes, transport gate passes (Fabric API holds the join until the client answers a configuration ping) |
| The same plus FabricProxy-Lite 2.10.1 (Velocity modern forwarding) | yes, transport gate passes with a fresh player record; FabricProxy-Lite asks for the player info twice and the proxy answers both as Velocity does. A second join of the same bot into a world that saved its player data received no chunk from the Fabric server even without the proxy (checked with a loopback tee), so repeat runs need the saved player data removed |
| NeoForge 21.4.161 on 1.21.4, no mods, forwarding none | yes, transport gate passes |
| Forge 54.1.22 on 1.21.4, no mods, forwarding none | yes, transport gate passes (once the server finished starting; a client that connects earlier is turned away by Forge itself) |
| Forge or NeoForge with modern forwarding, and any modpack | not tried; forwarding for them needs a forwarding mod and modpack compatibility is not claimed |

A relay of the configuration state is not a promise about any particular modpack.

## Network

| Case | Result |
|---|---|
| Listener on `::` with `dual-stack = true`: a server list ping | answered over `::1` and over `127.0.0.1` |
| A backend reached over IPv6 (`host = "::1"`) | a bot joined and played through a local forwarder that listened on `::1` and relayed to a Basalt Server |

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
| Other players: list, appear, move, leave, and what they say in chat | yes |
| Mobs appear with their Bedrock name and their hit box | yes (the bot does not report the box) |
| Inventory contents, after a `give` | yes |
| Time of day and rain | yes |
| Death, the death message and respawn | yes |
| Crossing to the Nether or the End (dimension change, heights) | dimension change seen by the bot; the Nether and End start at the bottom of the world |
| Game mode and the abilities that go with it (flight in creative) | game mode seen, abilities sent but not decoded by the bot |
| Plugin events, `player:send` and `player:kick` for a Bedrock player | yes |
| Breaking a block, and the block change that comes back | yes |
| Using an item on a block | yes, a block change comes back (which block it placed is not checked) |
| Attacking or using an entity, swinging the arm, sprinting | sent, the bot cannot do them |
| Dropped items | yes (counts and item ids seen) |
| Moving a Bedrock player to another backend (`player:connect`): the old world is cleared, the new one streams in, spawn again | yes, between two Basalt Servers |
| Moving a Bedrock player to a Paper 26.3 with Velocity forwarding and staying there | yes, with the Swarm bot; needs the proxy to end each position with the client tick packet and to send the address without the port |
| A Bedrock player served by a Basalt Server over the session envelope (`[bedrock] delegate = true`, backend `forwarding = "basalt"`) | yes, the bot joined through the proxy, spawned and received 81 chunks and the command list; moving that player to another backend is not supported |
| A Bedrock player on a backend older than 26.3 (`protocol = N` for N from 770 to 776, for example a Paper 1.21.8) | yes, the Swarm Bedrock bot joined real Paper 1.21.8, 26.2 and 1.21.11 through the proxy, saw chunks, the command list, block changes and the clock; the Java link of the bridge goes through the same climbing chains. Chaos replays of recorded 1.21.8 and 1.21.11 sessions with a Bedrock bot report nothing |
| A backend that requires a Java resource pack: the proxy accepts it for the Bedrock player (an optional pack is declined); the pack is not shown | yes, with Paper 26.3 and `require-resource-pack=true`; without it the join never finishes |
| The commands of the backend are offered to the Bedrock client so it accepts a slash command; each takes free text and no tab completion of arguments | yes, 102 commands seen by the bot on a Basalt Server, plus the 103rd of a proxy plugin |
| A command a proxy plugin registered with `cmd` run by a Bedrock player | yes, the bot ran one and got the plugin's answer |
| Worn armour and the off hand are shown to the Bedrock client (windows 120 and 119) | yes, the bot receives both windows with the items; the order of the armour slots follows the Java menu (head first) |
| Potion effects on the player (given, changed, cleared) | yes, the bot sees speed and regeneration added and removed; effects of other entities are not shown |
| Sneaking of a Bedrock player reaches the backend as a player input packet | yes, the packet is sent when the bot starts sneaking; the pose other players see was not checked |
| Selecting a hotbar slot as a Bedrock player | yes, one SetCarriedItem reaches the backend per change (seen with a TCP tee and the bot's `--hotbar`) |
| The hurt flash of the player and of mobs, and the death of a mob, shown to the Bedrock client | yes, the packets are sent and the bot accepts them; what the client draws was not checked |
| `Connect` sent by a backend on `bungeecord:main` (with `bungee-messages = true`) moves a Bedrock player; `GetServer`, `GetServers` and `UUID` are answered in the Bungee format | yes, messages injected between the proxy and the backend moved the bot and got the right replies |
| 150 Bedrock bots at once, release build | yes, all enter; 226 of 400 enter on this machine, where the bots and the backend use more CPU than the proxy (73% of one core) |
| Bedrock connections go through the same admission gate as Java ones (global and per-origin token buckets) | yes: 32 of 40 bots arriving at once from one address enter with the defaults |
| A RakNet peer that leaves splits open, holds ordered frames back or never acknowledges is cut off | yes (16 splits, 4 MiB of parts, 256 held frames, 8192 unacknowledged datagrams) |
| Titles, subtitles, the action bar | yes |
| Boss bars (create, rename, progress) | yes |
| The sidebar scoreboard (objective, title, lines) | yes |
| Mob custom names | sent, the bot does not read them |
| Baby mobs, burning, sneaking, invisible, gliding flags of mobs | sent, the bot does not read them |
| Open a chest, dispenser or hopper: the window opens at the clicked block, shows its slots and closes | yes |
| Open a furnace, blast furnace or smoker and move items into its three slots | yes, the bot put 5 iron ore into the ingredient slot; shulker boxes are shown as chests and were not tried |
| Sounds the backend names (a Basalt Server, `playsound`) are played under their Bedrock name | yes, the bot heard `random.orb` for the orb pickup; vanilla and Paper send most sounds as registry numbers, which are not translated |
| Move stacks between a window and the inventory, or inside the inventory (take, place, swap) | yes, checked with the bot in both directions; split stacks and drops are refused |
| Moving items inside an open window or the inventory | refused (the client reverts); the backend never sees it |
| Other windows (crafting, anvil...) are closed at once; mob colour and type variants | not done |

## Under stress

Under the ASan proxy with 100 Java bots (1.21.4, translated) on a Basalt Server, killing the backend halfway left the proxy serving, and a
termination signal ended it cleanly with no sanitizer report. Fuzzing of RakNet sessions, the envelope and the Bedrock packet readers (about
60,000 sessions and 200,000 inputs per seed) found nothing under ASan and UBSan.

## Limits that matter

- A client newer than its backend is not supported.
- Bedrock clients (protocol 2193) join the first backend of the default route as a Java client. See the Bedrock section.
- The official game client has not been tried against this proxy; every client above is a bot.
- Windows and macOS builds compile and the Windows one ran under wine; the macOS backend of the event loop ran on FreeBSD. On 2026-10-17 the whole tree still compiled for x86_64-windows-gnu, and for aarch64-macos except c-ares, which needs Apple frameworks; wine was not available that day, so nothing ran.

| Session envelope: Java clients delegated to a Basalt Server over data frames (`session-envelope = true` on the Server) | yes, 26.3, 1.21.4, 1.20.1, 1.16.5, plus Basalt to Paper and back |
