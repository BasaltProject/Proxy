# Extras a proxy is often asked for

None of these is needed by a contract already approved. Each is an opt-in extension for later, off by default, and none changes the core path.

| Extra | What exists today | Class |
|---|---|---|
| Server list answer | the proxy answers itself, from its configuration | core |
| Server list passthrough (the backend's own MOTD and counts) | not done | extension |
| Query (GameSpy 4 over UDP) | not done | extension |
| RCON | not done; the console is the only administration | extension |
| Remote administration (HTTP or a socket) | not done | extension |
| Balancing among backends | the default route is an ordered list and a refused or unreachable backend falls to the next; no weights, no least-loaded | extension |
| Per-host routes | done, `[forced-hosts]` | core |
| PROXY protocol in front of the listener | not done | extension |
| Limbo or a captcha before a backend | not done, and not a requirement | extension |
| Metrics export | counters in the console `status` only | extension |

A new extension gets its own configuration file, as the rest of the proxy does, and its own section in the compatibility matrix once checked.
