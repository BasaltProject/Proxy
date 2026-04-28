# Identity of a Bedrock player behind the proxy

A Bedrock account is not a Java account. The proxy never invents a Java premium identity for one.

## What the backend sees

| Part | Value |
|---|---|
| Name | `_` followed by the gamertag with every character outside letters, digits and `_` turned into `_`, at most 16 bytes |
| UUID | the offline UUID of that name (version 3, the one a Java server in offline mode gives that name), never a Mojang profile id |
| Skin | none: the backend gets no textures property |

The leading `_` keeps a gamertag from landing on a real Java name. Two gamertags that clean up to the same name get the same Java identity;
a network that cannot accept that should not let Bedrock in.

## What travels with it

| Backend forwarding | The XUID reaches the backend |
|---|---|
| Basalt (signed) | yes: the signed response says origin Bedrock and carries the XUID as the origin id |
| Velocity modern, BungeeCord, none | no: those formats have no place for it |

A backend that must tell a Bedrock player from a Java one has to use Basalt forwarding.

## Authentication

With `online-mode = true` a Bedrock client whose login chain is not signed by Xbox Live is turned away before any backend is chosen. With
`online-mode = false` an unsigned chain is let in and the name comes from what the client says, as for a Java client in offline mode.

## Not done

Floodgate-style linking of a Bedrock account to a Java account, skins, and a Bedrock player list that shows the real gamertag.
