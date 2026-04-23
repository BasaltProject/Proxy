# Basalt native forwarding, version 1

Wire format implemented in `wire/wire-auth` (`forwarding/BasaltForwarding.hpp`). Direction approved in
ADR-P003; rationale and threat comparison are in `Server/docs/forwarding-basalt.md`.

## Exchange

Inside the existing login, one round trip started by the backend on channel `basalt:forwarding`.
The handshake host is the real virtual host and is never modified.

## Request (backend to proxy)

| Field | Type | Rule |
|---|---|---|
| versionMin, versionMax | u8, u8 | min <= max; this revision reads and writes 1 |
| challenge | 32 bytes | CSPRNG, one per connection, consumed by that connection only |
| backendId | string, 1 to 64 bytes | the backend's own configured name |
| capabilities | varint bitset | what the backend offers; bits this revision does not know are ignored |

## Response (proxy to backend)

`signature` (Ed25519, 64 bytes) followed by the body:

| Field | Limit |
|---|---|
| version (varint) | within the offered range and 1 |
| keyId (string) | 16 hex characters |
| profileId (uuid), name (string) | name 1 to 16 |
| properties | at most 64; name 256, value 32768, signature 8192; strings in total 65536 |
| address, port (u16) | address 1 to 64 |
| virtualHost, virtualPort (u16) | host up to 255 |
| onlineMode (bool) | whether the proxy verified the player with Mojang |
| proxyId (string) | 1 to 64 |
| origin (varint), originId (string) | 0 Java, 1 Bedrock, 2 other; originId up to 64 |
| capabilities (varint) | subset of the offered set and of the known set; none are defined in version 1 |
| extraCount (varint) | reserved: must be 0 in version 1 |

The whole response is at most 128 KiB. The body must be consumed exactly.

## Signed message

```
"basalt:forwarding/1" 0x00 challenge backendId 0x00 body
```

`body` is every byte after the signature, so version and keyId are covered.

## Keys

Ed25519 raw 32-byte keys (RFC 8032, Monocypher 4.0.2 vendored in Wire). The proxy holds the private
seed, backends hold public keys only. `keyId` is the first 8 bytes of SHA-256 over the public key,
lowercase hex. Rotation: add the new public key to every backend, switch the proxy, remove the old one.

## Verification order

Size, then version and keyId from the body, then range, then trusted key lookup, then signature,
and only then the rest of the body. Every rejection carries a log-only reason.

## Not provided

The signature authenticates the proxy to the backend. It does not encrypt the link and must not be
described as doing so. It carries no permissions, chat key, locale or brand.

## Status

Codec and tests are in Wire, the backend endpoint in Server and the per-backend mode with key file in Proxy.
A capability is negotiated here but only activated by a later, separate step (P7).
