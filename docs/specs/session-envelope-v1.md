# Session envelope, version 1

Wire format in `wire/wire-protocol` (`SessionEnvelope.hpp`). It is the transport a Basalt Proxy and a Basalt Server share when the
Server, not the Proxy, translates a client's protocol. It is active for Java clients; see Status.

## Negotiation and activation are separate

Negotiation happens in the forwarding exchange (docs/specs/forwarding-basalt-v1.md). Capability bit 0, `kCapabilitySessionEnvelope`,
is offered by a backend in its signed request and selected by the proxy in its signed response only if the proxy implements the
envelope and its `translation.mode` allows delegating. A selected bit changes nothing by itself.

## Activation

After LoginFinished the proxy writes exactly one control frame `Open` as the last frame in
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

Codec, the Open/Opened state machine (`EnvelopeEndpoint`, one per peer) and tests are in Wire. Java is wired at both ends.

Proxy: it selects capability bit 0 in its signed response when the backend offers it, the backend is `forwarding = "basalt"` with `protocol = "client"` (or
`"auto"` resolving to it), the client is Java and `[translation] mode` is `auto`. Right after the backend's LoginFinished it sends `Open` (Java, the client's
protocol) as the last frame of the old codec, then wraps every packet it writes to that backend in a Data frame and unwraps what it reads; `Opened` is consumed.
Any other control frame, a malformed frame or `Close` ends that backend login. Each backend connection negotiates on its own, so a move Basalt to external,
external to Basalt and Basalt to Basalt needs nothing extra.

Server: `[network.forwarding] session-envelope = true` offers the bit; it answers `Opened`, adopts the protocol from `Open` and translates for it.

Checked with the Swarm Java bot through the Proxy against a Server built from its committed envelope adapter: 26.3, 1.21.4, 1.20.1 and 1.16.5 clients join and play;
`[translation] mode = "proxy"` selects nothing and the session is unchanged; Basalt to Basalt and Basalt to Paper to Basalt moves keep one client connection.

Not done: Bedrock (the Server answers `Close(Unsupported)`, and the Proxy never selects it for Bedrock players) and coverage-guided fuzzing of the framing.
