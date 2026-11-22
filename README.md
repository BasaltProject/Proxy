<p align="center">
  <a href="https://raw.githubusercontent.com/BasaltProject/.github/main/assets/basalt-logo-white.svg#gh-dark-mode-only">
    <img src="https://raw.githubusercontent.com/BasaltProject/.github/main/assets/basalt-logo-white.svg" alt="Basalt" width="360">
  </a>
  <a href="https://raw.githubusercontent.com/BasaltProject/.github/main/assets/basalt-logo-black.svg#gh-light-mode-only">
    <img src="https://raw.githubusercontent.com/BasaltProject/.github/main/assets/basalt-logo-black.svg" alt="Basalt" width="360">
  </a>
</p>

<h2 align="center">Basalt Proxy</h2>

<p align="center">
  <strong>A Minecraft proxy for Java and Bedrock, written from scratch in C++. No JVM required.</strong>
</p>

<p align="center">
  <img alt="Java Edition" src="https://img.shields.io/badge/Java_Edition-26.3-62B47A?logo=minecraft&logoColor=white">
  <img alt="Bedrock Edition" src="https://img.shields.io/badge/Bedrock_Edition-26.60-62B47A?logo=minecraft&logoColor=white">
  <img alt="C++" src="https://img.shields.io/badge/language-C%2B%2B-00599C?logo=cplusplus&logoColor=white">
  <img alt="Platforms" src="https://img.shields.io/badge/platforms-macOS_%7C_Linux_%7C_Windows-00599C">
  <img alt="License" src="https://img.shields.io/badge/license-GPL--3.0-green">
</p>

## About

Basalt Proxy sits in front of your Minecraft servers, gives players a single address and moves them between servers.
**Java Edition and Bedrock Edition players connect to the same proxy**, and it works with whatever is behind it:
vanilla, Paper, Spigot, Fabric or [Basalt Server](https://github.com/BasaltProject/Server).

| Edition | Version | Connection |
|---|---|---|
| Java Edition | 26.3 (older clients from 1.21.5 are translated) | Native |
| Bedrock Edition | 26.60 | Native, bridged to Java servers |

- **Native and light.** One binary and a `config/` folder. No JVM to install.
- **Built-in protection.** Antibot, secure forwarding to backends, rate limits and a remote administration API.
- **WebAssembly plugins.** The native API, sandboxed and packaged as `.bppkg`.
- **BungeeCord and Velocity plugins (early).** Optional adapters on top of [Shim](https://github.com/BasaltProject/Shim),
  a JVM built into the binary that only starts when a jar is present.

## Getting started

Download a binary from the [releases page](https://github.com/BasaltProject/Proxy/releases) (stable builds and a
rolling **Nightly**), run it once to generate `config/`, add your servers to `servers.toml` and start it again.

Configuration, forwarding, plugins and everything else are in the [documentation](https://basaltmc.org/docs/proxy/introduction).

## Building

```bash
git clone --recurse-submodules https://github.com/BasaltProject/Proxy.git
cd Proxy
cmake --preset release
cmake --build build/release
```

Needs CMake 3.28+, Ninja and Clang. The binary ends up in `build/release/bin/`.

## Contributing

Read the [contribution guide](CONTRIBUTING.md) and the [Code of Conduct](.github/CODE_OF_CONDUCT.md).
Report vulnerabilities privately through the [security policy](SECURITY.md).

## License

[GPL-3.0](LICENSE). Basalt is not affiliated with Mojang Studios or Microsoft. Minecraft is a trademark of Mojang Studios.
