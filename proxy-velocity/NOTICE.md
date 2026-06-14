# Notice about the Velocity adapter

This module adapts the shape of the published Velocity plugin API onto Basalt Proxy's native services.

- Class, method and event names follow the published API, because plugins that expect it require them to match.
- The logic lives in native code (`proxy-plugins`) and in the runtime. This module only maps names and shapes
  and holds no mechanics of its own.
- Nothing here is Velocity source code. Bodies are written from the documented behavior.
- Java plugins run on Basalt Shim through JNI, built with a small constructor injector instead of Guice. The Velocity
  API jar and its dependencies are downloaded by `scripts/fetch-deps.sh`, checked against their published SHA-1, and are
  never stored in this repository.

Basalt is not affiliated with, endorsed by or sponsored by the Velocity project or its maintainers.
