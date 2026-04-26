# Proxy plugin API, version 1

The proxy runs Lua plugins with the same shape the server uses: the same manifest, the same globals for the same ideas,
the same sandbox. A plugin written for one reads like a plugin written for the other; only the domain differs (the proxy
has players and servers, the server has worlds and entities).

Status: implemented in `proxy-plugins`. Everything marked *not yet* is deliberately absent, not forgotten.

## Package

A plugin is a folder or a `.bppkg` (a zip) in the plugins directory. At its root:

```toml
id = "hello"            # lowercase, the identity of the plugin
name = "Hello"
version = "1.0.0"
api = 1
main = "src/main.lua"   # default

[capabilities]
storage = true          # `store` (a key-value table for the plugin)
players_data = true     # `player.data` (the same, per player)
commands = true         # `run`, and the operator must grant it in plugins.toml
# network = ["channel"], packets = true: reserved, not used by the proxy yet

[depends]
other_plugin = ">=1.0"

[exports]
modules = ["shared"]    # modules other plugins may `require`
```

`resources/` ships with the package; its files are copied to `<plugins>/<id>/` on first load, and `*.toml` files there
become the global `config` (`config.toml` at the top level, any other file as a table named after it). Edit them in the
plugin's folder, not in the package.

Permissions an operator grants live in `plugins.toml`:

```toml
operators = ["Steve"]          # may run commands a plugin guards with a permission

[allow]
hello = ["commands"]
```

A plugin that asks for a permission nobody granted does not load.

## Sandbox

One thread runs every plugin. A handler gets an instruction budget per run (`instructions-per-turn`) and the plugin a memory
ceiling (`memory-mib`). A handler that runs out of either, or fails five times in a row, disables its plugin until the next
reload; the proxy and the other plugins keep going. `os.execute`, `io.popen` and `package.loadlib` do not exist.

## Globals

### Identity and configuration

| Global | |
|---|---|
| `plugin.id`, `plugin.name`, `plugin.version` | The manifest. |
| `log.info(text)`, `log.warn(text)`, `log.error(text)` | Writes to the proxy's log under the plugin's name. |
| `config` | Read-only table built from the plugin's `*.toml` files. Keys with `-` are also readable with `_`. |

### Events

```lua
on(event, [options], function(e) ... end)
watch(event, [options], function(e) ... end)   -- observer: same events, never changes the outcome
```

`options` may carry `priority` (`lowest`, `low`, `normal`, `high`, `highest`, `monitor`) and a filter on the event's own
fields (`player_name`, `player_uuid`, `server`, `from_server`).

| Event | Fields |
|---|---|
| `plugin.enable` | Once, on the plugin thread, after every plugin has loaded. May call `wait`. `on` only. |
| `plugin.disable` | Once on reload or shutdown. `on` only. |
| `player.join` | `player`, `player_name`, `player_uuid`, `server`. The player is in play on their first backend. |
| `player.quit` | The same; `server` is where they were. |
| `player.server_change` | The same, plus `from_server`. |
| `player.chat` | The same, plus `message`. Seen once the player sent it; it cannot be cancelled. Unsigned and signed chat both. |
| `player.kick` | The same, plus `reason` in plain text. A backend disconnected the player in play; it cannot be cancelled. |
| `server.ping` | `host` (what the client typed) and `protocol`. A server-list request was answered; there is no player and it cannot be cancelled. |
| `player.login` | `player_name`, `player_uuid`. The player is authenticated and no backend has been chosen yet; it cannot be cancelled. |

No event of the proxy is cancellable yet, so `on` and `watch` differ only for the `plugin.*` events.
*Not yet:* cancelling any event.

### Commands

```lua
cmd("coins top [limit: int 1..25]", function(src, limit) ... end, { permission = "shop.top" })
```

The spec is the server's: words first, then arguments in `<required: type>` or `[optional: type]` form; types are `word`,
`text` (the rest of the line), `int` (with an optional `min..max`), `float` and `bool`. The handler receives the source,
then one value per argument, `nil` for an optional one that was not given. A mismatch answers the sender with the usage.

A command is available from the console and from players. `src` is a player table or the console:
`src.is_console`, `src.name`, `src:send(text)`. `permission` restricts a command to the operators listed in `plugins.toml`
for players; the console always may. The proxy's own commands (`help`, `servers`, `status`, `reload`, `send`, `stop`,
`plugins`, reserved) cannot be replaced.
Clients 1.13 and later complete the command from their command tree, with the declared argument types and ranges.

### Players

```lua
players.online()        -- list of player tables
players.by_name(name)   -- case-insensitive; nil when not online
players.by_uuid(uuid)
players.count()
broadcast(text)
```

A player table has `name`, `uuid`, `server`, `is_console = false` and:

| | |
|---|---|
| `player:send(text)` | Chat line to that player. |
| `player:connect(server)` | Moves them to a backend without reconnecting; returns whether it was accepted. |
| `player:kick(reason)` | Disconnects them; the reason is MiniMessage. |
| `player.data.key` | Persistent, per player and plugin. Needs `players_data`. Assigning `nil` deletes. |

*Not yet:* ping, address, client version, titles and action bars.

### Servers

```lua
servers.list()          -- names of the backends in backends.toml
servers.exists(name)
```

### Text

`send` and `broadcast` take MiniMessage (`<gradient:aqua:green>Hi</gradient>`, `<red>`, `<click:...>`). Plain text works as is.
Clients before 1.19 receive no plugin chat lines yet.

Lines of text live in `lang/<locale>.toml` inside the plugin (nested tables flatten with dots). A message that starts with `@`
is a key (`player:send('@shop.bought', {item = 'apple'})`), `t('shop.bought', {item = 'apple'})` returns the line, and `{name}`
takes a field of the table passed last. Only `en_us` is the default locale for now; `send`, `broadcast`, `kick` and the console
`send` all resolve keys.

### Time

Durations are in ticks of 50 ms, as on the server.

```lua
later(ticks, fn)                 -- returns a handle with :cancel()
every([delay,] period, fn)       -- the same, repeating
wait(ticks)                      -- inside a handler or command: yields, the thread keeps serving others
```

### Storage

`store.get(key)`, `store.set(key, value)`, `store.delete(key)`, `store.keys()`. Values are strings, numbers or booleans;
keys are at most 128 bytes, values 64 KiB, and a plugin holds at most 1000 keys (the same per player). Needs `storage`.
The file is the proxy's own `.basalt/proxy.db`; it is state, never shared with a server.

### Console

`run(line)` runs a console command and returns whether it named one. It needs `commands`, granted by the operator. Today it
reaches the plugins' commands; the proxy's built-ins are not reachable from a plugin.

## Differences from the server, on purpose

- No `worlds`, `menu`, `placeholder`, `packet` or `channel`: the proxy has no worlds or inventories, and its packet and
  channel surface is reserved for the `packets` and `network` permissions.
- `cmd` runs from the console too, because the proxy has no other way to be driven.
- Players are looked up from what the proxy announced, so a player appears in `players.online()` once they are in play.
