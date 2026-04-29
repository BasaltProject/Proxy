# Bedrock translation: what Server does, what the proxy does

Server serves a Bedrock player from its own world (about 5,600 lines in `server-runtime/src/bedrock`). The proxy has no world: it reads a
Java backend's packets and writes Bedrock's (about 1,300 lines in `proxy-runtime/src/bedrock`). The parts that do not touch a world live
in `wire-bedrock`.

## Inventory

| Area | Server | Proxy | In Wire |
|---|---|---|---|
| Transport, batching, encryption, login chain | `BedrockListener` | the same classes | `RakServer`, `RakSession`, `BedrockConnection` |
| Start of the game (start game, biomes, items, entity ids, creative, rules) | `BedrockListener::onReady` | `BedrockSession::joined` | writers in `BedrockPackets`, item and entity tables |
| Chunks | `BedrockChunks` reads a Server `Chunk` | `BedrockSession::chunk` decodes a Java column | `encodeColumn`, block, biome and item tables |
| Blocks | per change from its world | Java block updates | `bedrockBlockId` for 26.3 |
| Movement and rotation | `BedrockPlayer` | position packets both ways | `PlayerAuthInput` reader |
| Chat, commands, forms | `BedrockPlayer`, forms in `BedrockPlayerConnection` | text and command request; no forms | text and form packets |
| Other players and mobs | `BedrockSeen` from its entities | spawn, move and remove packets | add player, add entity, player list |
| Dropped items | `BedrockSeen` | spawn plus entity data | add item entity |
| Inventory and windows | `BedrockWindows`, `BedrockStackRequests` | the 36 carried slots, read only | inventory content writer |
| Crafting recipes, maps, shield, records | `BedrockRecipes`, `BedrockMaps`, `BedrockShield`, `BedrockRecords` | none | recipe and map writers |
| Health, hunger, experience | `BedrockPlayerConnection` | attribute updates from the Java health and experience packets | attribute writer |
| Titles and the action bar | `BedrockPlayerConnection` | from the Java title and chat overlay packets | `writeSetTitle` |
| Boss bars | `BedrockPlayerConnection` | create, remove, progress and name | `writeBossEvent` |
| The sidebar scoreboard | `BedrockPlayerConnection` | objective, display and score packets, for the sidebar only | scoreboard writers |
| Effects | `BedrockPlayerConnection` | none | writers exist |

## What stays in Server

Anything that reads Server's own player, world or entity objects: windows and stack requests (they run Server's rules for what a click
does), recipes, maps, effects and the rest of the player state. Moving them means giving them an interface over a domain that is not Server's.

## What the proxy still lacks

Open containers (a chest, a furnace), item stack requests in either direction, crafting, effects, forms and particles, and the tab list and below-name scoreboards. A Bedrock player behind the proxy can walk, see, chat, run commands, build, switch backend and carry items,
not play survival through windows.
