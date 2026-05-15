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

The envelope endpoint, the frame decoder and the forwarding request were fuzzed with libFuzzer under ASan and UBSan (2.8 million runs, no finding).

Not done: Bedrock end to end. The Proxy half exists (it selects the capability for a Bedrock player only when `[bedrock] delegate = true` and the backend uses
`forwarding = "basalt"`, opens the envelope with the identity and relays packets); the Server answers `Close(Unsupported)` until its half exists.

## Bedrock (contract agreed with the Server; the Proxy half is in behind `[bedrock] delegate = true`)

The same frames carry a Bedrock player; what changes is what a Data frame holds, what comes before it and who does what.

- The Proxy keeps the transport and the identity. It runs the whole Bedrock handshake itself: RakNet, network settings, the login chain, encryption and the
  resource pack exchange up to the client's final "completed" response. Only then does it open the envelope, so the first Data frame the backend sees is the
  client's first packet after that and the backend begins with StartGame.
- Opening: `Open` with edition Bedrock and the client's Bedrock protocol, then one `BedrockIdentity` control frame (type 3), then the backend answers `Opened`.
  The identity holds the gamertag, the xuid, whether the login chain was authenticated, the client's address, the skin id, the skin's resource patch (geometry
  JSON), its width and height and its raw RGBA pixels (width times height times four bytes, each side at most 1024). It is the only control frame that may
  exceed 512 bytes, up to the 2 MiB of a Data frame. An `Opened` before the identity ends the exchange.
- A Data frame holds one Bedrock game packet as the client's protocol frames it (packet id and body), after the Proxy has decrypted and unbatched it. Going the
  other way the backend sends one frame per packet, in order, and the Proxy batches, compresses and encrypts; there is no flush marker. RakNet frames, datagrams and
  the batch header never cross.
- The backend runs what it already runs for a Bedrock player it serves directly, over frames instead of a RakNet session: its translation, its chunks and its
  windows. The Proxy only relays, so it needs no Java translation for that session.
- A backend that ends a session sends `Disconnect` (control type 4) with a message of at most 400 bytes; the Proxy turns it into a Bedrock Disconnect packet.
  `Close` keeps its four reasons for protocol failures.
- Limits and failures are the Java ones: 2 MiB per data frame, a frame that is neither `Opened`, `Disconnect` nor `Close` where it does not belong ends the login,
  and `Close` ends the session.
- Compatibility: a backend that does not offer the capability, or answers `Close(Unsupported)`, keeps today's behaviour, in which the Proxy translates the Bedrock
  player to a Java backend itself.
