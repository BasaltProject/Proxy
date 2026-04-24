# Session envelope, version 1

Wire format in `wire/wire-protocol` (`SessionEnvelope.hpp`). It is the transport a Basalt Proxy and a Basalt Server will share when the
Server, not the Proxy, translates a client's protocol (ADR-P001). The codec exists and is tested; nothing activates it yet.

## Negotiation and activation are separate

Negotiation happens in the forwarding exchange (docs/specs/forwarding-basalt-v1.md). Capability bit 0, `kCapabilitySessionEnvelope`,
is offered by a backend in its signed request and selected by the proxy in its signed response only if the proxy implements the
envelope and its `translation.mode` allows delegating. Today the Proxy never selects it, so no session uses the envelope. A selected
bit changes nothing by itself.

## Activation (to be implemented)

After LoginFinished, in the transport direction of each peer, the sender writes exactly one control frame `Open` as the last frame in
the old codec and every frame after it is an envelope frame. The receiver answers `Opened`, and until it arrives only `Opened` and
`Close` are legal. A peer that receives anything else, an unknown version, or a second `Open` closes the connection.

## Frames

| Byte 0 | Meaning | Rest |
|---|---|---|
| 0 | Data | one original client packet, unmodified, as the client's protocol framed it (id and body) |
| 1 | Control | one byte of type, then the message |

Control messages: `Open` (type 0): version u8, edition u8 (0 Java, 1 Bedrock), original protocol varint. `Opened` (type 1): version u8.
`Close` (type 2): reason u8 (0 normal, 1 unsupported, 2 malformed, 3 superseded).

## Limits

A data frame is at most 2 MiB; a control frame at most 512 bytes; both measured before any outer compression. Trailing bytes, an
unknown edition or reason, a negative protocol and an unknown kind or control type are malformed. Decoding never reads past the frame.

## What it does not do

It does not encrypt, authenticate each frame or replace the link's confidentiality, which stays the job of the transport (private
network, tunnel). It carries no permissions, and the Server must not read any identity from it beyond what the signed forwarding
already proved. Bedrock datagrams are not Java packets: for Bedrock the data frames carry game payloads after RakNet, never RakNet itself.

## Status

Codec and a round-trip/limits test are in Wire. Not done: the Open/Opened handshake in both peers, the Server adapter that runs
translation on data frames, switching from Basalt to external backends and back, and forced-mode configuration.
