# Proxy plugin example

A small plugin for Basalt Proxy's Lua API, the proxy counterpart of ServerPlugin-Example: it greets a player with a visit counter kept in the proxy's
own database, answers `/where`, and sends a player to a configured backend with `/lobby`. Zip the contents of this folder as `example.bppkg` (the `lang` files and `config.toml` under `resources/` are read from a package), put it in the proxy's
`plugins/` directory and switch plugins on in `config/plugins.toml`.

The API is in `docs/specs/plugin-api-v1.md`, and its LuaLS definitions in `docs/specs/basalt-proxy.d.lua`.
