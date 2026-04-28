---@meta
-- Types for the Basalt Proxy Lua API, version 1. For editors: LuaLS and EmmyLua read these annotations.

---@class Plugin
---@field id string
---@field name string
---@field version string
plugin = {}

---@class Log
local Log = {}
---@param text any
function Log.info(text) end
---@param text any
function Log.warn(text) end
---@param text any
function Log.error(text) end
---@type Log
log = {}

---Read-only. The plugin's `*.toml` files; keys with `-` are also readable with `_`.
---@type table<string, any>
config = {}

---@class Player
---@field name string
---@field uuid string
---@field server string The backend the player is on.
---@field is_console boolean false for a player
---@field data table<string, string|number|boolean|nil> Needs `players_data`. Assigning nil deletes.
local Player = {}
---Chat line in MiniMessage.
---@param text string
function Player:send(text) end
---Disconnects the player. The reason is MiniMessage.
---@param reason? string
function Player:kick(reason) end
---Moves the player to a backend without reconnecting.
---@param server string
---@return boolean accepted
function Player:connect(server) end

---@class ConsoleSource
---@field is_console boolean true
---@field name string "CONSOLE"
local ConsoleSource = {}
---@param text string
function ConsoleSource:send(text) end

---@alias EventPriority "FIRST"|"EARLY"|"NORMAL"|"LATE"|"LAST"

---@class EventOptions
---@field priority? EventPriority
---@field player_name? string
---@field player_uuid? string
---@field server? string
---@field from_server? string

---@class PlayerEvent
---@field player Player
---@field player_name string
---@field player_uuid string
---@field server string
---@field from_server? string Only on `player.server_change`.
---@field message? string Only on `player.chat`.
---@field reason? string Only on `player.kick`.

---@class PingEvent
---@field host string
---@field protocol integer

---@param event "plugin.enable"|"plugin.disable"
---@param handler fun(e: table)
---@overload fun(event: "player.login", handler: fun(e: {player_name: string, player_uuid: string}))
---@overload fun(event: "server.ping", handler: fun(e: PingEvent))
---@overload fun(event: "player.join"|"player.quit"|"player.server_change"|"player.chat"|"player.kick", handler: fun(e: PlayerEvent))
---@overload fun(event: "player.join"|"player.quit"|"player.server_change"|"player.chat"|"player.kick", options: EventOptions, handler: fun(e: PlayerEvent))
function on(event, handler) end

---@param event "player.join"|"player.quit"|"player.server_change"|"player.chat"|"player.kick"
---@param handler fun(e: PlayerEvent)
---@overload fun(event: "player.join"|"player.quit"|"player.server_change"|"player.chat"|"player.kick", options: EventOptions, handler: fun(e: PlayerEvent))
function watch(event, handler) end

---@class CommandOptions
---@field permission? string Players need to be listed under `operators` in plugins.toml.

---Words, then `<required: type>` or `[optional: type]` arguments; types: word, text, int (min..max), float, bool.
---@param spec string
---@param handler fun(src: Player|ConsoleSource, ...: string|number|boolean|nil)
---@param options? CommandOptions
function cmd(spec, handler, options) end

---@class Players
local Players = {}
---@return Player[]
function Players.online() end
---@param name string
---@return Player|nil
function Players.by_name(name) end
---@param uuid string
---@return Player|nil
function Players.by_uuid(uuid) end
---@return integer
function Players.count() end
---@type Players
players = {}

---@class Servers
local Servers = {}
---@return string[]
function Servers.list() end
---@param name string
---@return boolean
function Servers.exists(name) end
---@type Servers
servers = {}

---Chat line in MiniMessage to every player.
---@param text string
function broadcast(text) end

---Runs a console command. Needs `commands`, granted in plugins.toml.
---@param line string
---@return boolean named
function run(line) end

---@class Handle
local Handle = {}
function Handle:cancel() end

---@param ticks integer 1 tick is 50 ms
---@param fn fun()
---@return Handle
function later(ticks, fn) end

---@param period integer
---@param fn fun()
---@return Handle
---@overload fun(delay: integer, period: integer, fn: fun()): Handle
function every(period, fn) end

---Yields the current handler or command for this many ticks.
---@param ticks? integer
function wait(ticks) end

---@class Store
local Store = {}
---@param key string
---@return string|number|boolean|nil
function Store.get(key) end
---@param key string
---@param value string|number|boolean
---@return boolean
function Store.set(key, value) end
---@param key string
---@return boolean
function Store.delete(key) end
---@return string[]
function Store.keys() end
---Needs `storage` in the manifest.
---@type Store
store = {}

---@param key string
---@param arguments? table
---@return string
function t(key, arguments) end
