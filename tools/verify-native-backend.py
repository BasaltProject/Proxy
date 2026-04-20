#!/usr/bin/env python3
"""Exercise an already running offline 26.3 backend with Proxy and independent Swarm.

Only loopback endpoints are used. No server jar, world, EULA or credentials are created.
The transparent frontend recorder independently decodes framing/zlib and counts packets.
This is a bot transport gate, not validation of a graphical client or chunk semantics.
"""
import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import socket
import uuid
import zlib


def integer(data, offset=0):
    value = 0
    for shift in range(0, 35, 7):
        if offset >= len(data):
            return None
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
    raise ValueError("oversized VarInt")


class Recorder:
    def __init__(self):
        self.pending = {"client": bytearray(), "server": bytearray()}
        self.handshake = True
        self.phase = "login"
        self.threshold = None
        self.counts = {}
        self.profile = None
        self.maximum = 0
        self.keepalives = set()
        self.teleports = set()
        self.matched_keepalives = 0
        self.matched_teleports = 0

    def feed(self, direction, chunk):
        data = self.pending[direction]
        data.extend(chunk)
        while (header := integer(data)) is not None:
            length, start = header
            if start > 3 or not 0 < length <= 2097151:
                raise ValueError("invalid frame length")
            if len(data) < start + length:
                break
            body = bytes(data[start:start + length])
            del data[:start + length]
            if self.threshold is not None:
                size, offset = integer(body)
                if size > 8388608:
                    raise ValueError("inflated packet exceeds limit")
                encoded = body[offset:]
                if size:
                    decoder = zlib.decompressobj()
                    body = decoder.decompress(encoded, size + 1)
                    if len(body) != size or not decoder.eof or decoder.unused_data:
                        raise ValueError("invalid compressed frame")
                else:
                    body = encoded
            self.maximum = max(self.maximum, len(body))
            packet_id, offset = integer(body)
            if direction == "client" and self.handshake:
                protocol, _ = integer(body, offset)
                if packet_id != 0 or protocol != 777:
                    raise ValueError("expected native 26.3 handshake")
                self.handshake = False
                continue
            key = f"{self.phase}/{direction}/{packet_id:#x}"
            self.counts[key] = self.counts.get(key, 0) + 1
            if self.phase == "login":
                if direction == "server" and packet_id == 3:
                    self.threshold, _ = integer(body, offset)
                elif direction == "server" and packet_id == 2:
                    profile_id = str(uuid.UUID(bytes=body[offset:offset + 16]))
                    size, name_at = integer(body, offset + 16)
                    name = body[name_at:name_at + size].decode("utf-8")
                    self.profile = {"uuid": profile_id, "name": name}
                elif direction == "client" and packet_id == 3:
                    self.phase = "configuration"
            elif self.phase == "configuration" and direction == "client" and packet_id == 3:
                self.phase = "play"
            elif self.phase == "play":
                if direction == "server" and packet_id == 0x2d:
                    token = body[offset:]
                    if len(token) != 8:
                        raise ValueError("invalid keepalive size")
                    self.keepalives.add(token)
                elif direction == "client" and packet_id == 0x1c:
                    token = body[offset:]
                    if token not in self.keepalives:
                        raise ValueError("unmatched keepalive response")
                    self.keepalives.remove(token)
                    self.matched_keepalives += 1
                elif direction == "server" and packet_id == 0x49:
                    token, _ = integer(body, offset)
                    self.teleports.add(token)
                elif direction == "client" and packet_id == 0:
                    token, end = integer(body, offset)
                    # 26.3 confirms with teleport ID, three doubles and two floats.
                    if token not in self.teleports or end + 32 != len(body):
                        raise ValueError("unmatched teleport confirmation")
                    self.teleports.remove(token)
                    self.matched_teleports += 1


def unused_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


async def stop(process):
    if process is None or process.returncode is not None:
        return
    process.terminate()
    try:
        await asyncio.wait_for(process.wait(), 5)
    except asyncio.TimeoutError:
        process.kill()
        await process.wait()


async def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    config = output / "config"
    config.mkdir(exist_ok=True)
    proxy_port = unused_port()
    (config / "proxy.toml").write_text(
        f'[network]\nbind = "127.0.0.1"\nport = {proxy_port}\n'
        'online-mode = false\ncompression-threshold = 256\n')
    backend_config = ('[routing]\ndefault = ["native"]\n[backends.native]\nhost = "127.0.0.1"\n'
                      f'port = {args.backend_port}\nprotocol = 777\nforwarding = "{args.forwarding}"\n')
    if args.secret_file:
        secret = args.secret_file.read_text().rstrip("\r\n")
        if not secret or len(secret.encode("utf-8")) > 4096 or "\0" in secret:
            raise ValueError("invalid forwarding secret file")
        backend_config += f'secret = {json.dumps(secret)}\n'
    (config / "backends.toml").write_text(backend_config)
    (config / "backends.toml").chmod(0o600)
    recorder = Recorder()
    failures = []
    connections = set()

    async def relay(reader, writer):
        connections.add(asyncio.current_task())
        upstream = None
        try:
            upstream_reader, upstream = await asyncio.open_connection("127.0.0.1", proxy_port)

            async def pump(source, target, direction):
                while chunk := await source.read(65536):
                    recorder.feed(direction, chunk)
                    target.write(chunk)
                    await target.drain()

            pumps = [asyncio.create_task(pump(reader, upstream, "client")),
                     asyncio.create_task(pump(upstream_reader, writer, "server"))]
            _, pending = await asyncio.wait(pumps, return_when=asyncio.FIRST_COMPLETED)
            for task in pending:
                task.cancel()
            results = await asyncio.gather(*pumps, return_exceptions=True)
            failures.extend(str(item) for item in results if isinstance(item, Exception))
        except Exception as error:
            failures.append(str(error))
        finally:
            writer.close()
            if upstream:
                upstream.close()
            connections.discard(asyncio.current_task())

    proxy = swarm = None
    survived = False
    proxy_log = (output / "proxy.log").open("wb")
    swarm_log = (output / "swarm.log").open("wb")
    try:
        proxy = await asyncio.create_subprocess_exec(str(args.proxy.resolve()), "--config", str(config),
                                                     stdout=proxy_log, stderr=asyncio.subprocess.STDOUT)
        for _ in range(200):
            if proxy.returncode is not None:
                raise RuntimeError("proxy exited during startup")
            try:
                _, writer = await asyncio.open_connection("127.0.0.1", proxy_port)
                writer.close()
                await writer.wait_closed()
                break
            except OSError:
                await asyncio.sleep(0.05)
        else:
            raise RuntimeError("proxy startup timed out")
        async with await asyncio.start_server(relay, "127.0.0.1", 0) as tap:
            port = tap.sockets[0].getsockname()[1]
            swarm = await asyncio.create_subprocess_exec(
                str(args.swarm.resolve()), "--ip", f"127.0.0.1:{port}", "--count", "1",
                "--version", "26.3", "--no-physics", stdout=swarm_log, stderr=asyncio.subprocess.STDOUT)
            try:
                await asyncio.wait_for(swarm.wait(), args.duration)
            except asyncio.TimeoutError:
                survived = True
            await stop(swarm)
            if connections:
                await asyncio.wait_for(asyncio.gather(*list(connections)), 5)
    finally:
        await stop(swarm)
        await stop(proxy)
        proxy_log.close()
        swarm_log.close()
    expected = uuid.UUID(bytes=hashlib.md5(b"OfflinePlayer:Bot_1", usedforsecurity=False).digest(), version=3)
    required = ["login/server/0x2", "configuration/server/0x7", "play/server/0x32",
                "play/server/0x2e", "play/server/0x49", "play/server/0x2d", "play/client/0x1c"]
    if args.expect_login_rejection:
        required = ["login/server/0x0"]
    missing = [key for key in required if not recorder.counts.get(key)]
    bot_log = (output / "swarm.log").read_text(errors="replace")
    diagnostics = [line for line in bot_log.splitlines() if line.startswith("Bot 1: ")]
    if args.expect_login_rejection:
        passed = (not failures and not missing and recorder.profile is None
                  and not any(key.startswith("play/") for key in recorder.counts)
                  and "disconnected in Login" in bot_log)
    else:
        passed = (survived and not failures and not missing
                  and recorder.matched_keepalives > 0 and recorder.matched_teleports > 0
                  and recorder.profile == {"uuid": str(expected), "name": "Bot_1"}
                  and "joined the game (Play State)" in bot_log)
    report = {"gate": "login-rejection" if args.expect_login_rejection else "native-offline-transport",
              "passed": passed, "protocol": 777, "forwarding": args.forwarding, "online": False,
              "expected_login_rejection": args.expect_login_rejection,
              "frontend_compression": 256, "survived_window": survived, "requested_seconds": args.duration,
              "profile": recorder.profile, "packets": recorder.counts, "bot_diagnostics": diagnostics,
              "max_decoded_packet_bytes": recorder.maximum, "missing": missing, "errors": failures,
              "matched_keepalives": recorder.matched_keepalives, "matched_teleports": recorder.matched_teleports,
              "limits": ("Independent bot/frame recorder; registry/chunk semantics, graphical client "
                         "and online authentication unverified.")}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--proxy", type=Path, required=True)
    parser.add_argument("--swarm", type=Path, required=True)
    parser.add_argument("--backend-port", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--duration", type=float, default=22)
    parser.add_argument("--forwarding", choices=["none", "velocity"], default="none")
    parser.add_argument("--secret-file", type=Path)
    parser.add_argument("--expect-login-rejection", action="store_true")
    options = parser.parse_args()
    if (options.forwarding == "velocity") != (options.secret_file is not None):
        parser.error("velocity requires --secret-file; none cannot use a secret")
    if not 1 <= options.backend_port <= 65535 or not 16 <= options.duration <= 120:
        parser.error("backend port must be 1..65535 and duration 16..120 seconds")
    raise SystemExit(asyncio.run(run(options)))
