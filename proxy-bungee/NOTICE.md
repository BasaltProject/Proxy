# Notice about the BungeeCord adapter

This module adapts the shape of the published BungeeCord plugin API onto Basalt Proxy's native services.

- Class, method and event names follow the published API, because plugins that expect it require them to match.
- The logic lives in native code (`proxy-plugins`) and in the runtime. This module only maps names and shapes
  and holds no mechanics of its own.
- Nothing here is BungeeCord source code. Bodies are written from the documented behavior.
- Java plugins run on Basalt Shim through JNI. The BungeeCord API jars are downloaded by `scripts/fetch-deps.sh`, checked
  against their published SHA-1, and are never stored in this repository.

Basalt is not affiliated with, endorsed by or sponsored by the BungeeCord project or its maintainers.
