# Upward translation: a current client on an older server

A client on the newest protocol (777, Minecraft 26.3) can be carried to a Java backend on an older one. It is the way back of what
the proxy already does for older clients: one **step** per version, each step reading the server's packets in its own numbering and
writing the next version's. The route is chosen per backend with `protocol = N` in `backends.toml`; the proxy refuses a pair it has
no route for before any state is entered.

## Shape

- `wire-multiversion` keeps one table set per step (`Tables776` is 26.3 to 26.2, and so on). A step going up reads the same table inverted
  (`invertedTables`): the runs of constant difference are bijections, so their inverse is exact, and what the older version has no name
  for is simply absent.
- A step is a `ProtocolLink` (`Link776Up`, `Link775Up`, `Link774Up`, `Link773Up`, `Link772Up`). Clientbound, the chain runs the steps from the
  server's side to the client's; serverbound it runs them the other way, each step seeing a packet by its own numbering
  (`ProtocolChain` built with `stepwise = true`). A step may claim a client packet its server has no id for
  (`serverboundForeign`, used for the attack and spectate packets of 26.1).
- On the client's side of the last step, `UpEntityData` puts entity data in this build's layout (serializer ids, indices a step opened
  again) and `ContentRewriter` renumbers blocks, items, particles and the rest through the composed tables. It also adds the brewing
  stand's item lists, which older servers never sent.
- Registries that grew are added from bodies a real server of the newer version sends (`NewRegistries77x`), and the ones that changed
  shape are rewritten entry by entry. Each rewrite was compared with what a real server of the newer version sends for the same data
  (a scratch program that converts the captured registry packets and diffs them as NBT, ignoring key order): what is left are vanilla
  data changes between versions.
- The runtime does it at the backend's edge (`ClientSession::upgradeFrame` and `downgradeFrame`), so everything above the edge
  (plugins, keep-alives, packs, cookies, the command relay) sees this build's numbering whatever the backend speaks.

## What is checked

Against real Paper servers of 26.2, 26.1.2, 1.21.11 and 1.21.10 with the Swarm bot and an independent frame recorder: the join, keep-alives,
teleport confirmation, the command tree identical to what a bot of the server's own version sees, commands, summoning mobs, and moves between
two servers. The bot also checks registries, their elements, tags and command argument registries against the target version: nothing is
reported on these routes. No graphical client has been tried.

## What is not converted

- Item components whose shape changed (a trim with an inline material, pot decorations, teleport effects on consumables, and so on).
- The section fluid count when the server is older than 26.1: it is the number of blocks, which can only be too high.
- Wolf and bee anger times and the avatar's main hand, whose entity data changed type in 1.21.11: from a 1.21.9 or 1.21.10 server they are dropped, because a client
  that gets another type for a field disconnects.
- Anything older than 1.21.7: not carried yet. The 1.21.7 step itself covers the player's entity data moving indices and the shoulder parrots changing
  from a compound to a variant (through a hook of the entity data reader), and the border centre divided by the dimension's coordinate scale.
