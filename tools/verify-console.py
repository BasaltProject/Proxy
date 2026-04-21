#!/usr/bin/env python3
"""Exercise console input and shutdown against a built proxy on loopback."""

import argparse
import json
import pathlib
import signal
import socket
import subprocess
import tempfile
import time


def varint(value):
    result = bytearray()
    while True:
        part = value & 127
        value >>= 7
        result.append(part | (128 if value else 0))
        if not value:
            return bytes(result)


def exact(peer, count):
    result = bytearray()
    while len(result) < count:
        chunk = peer.recv(count - len(result))
        if not chunk:
            raise RuntimeError("status probe reached EOF")
        result.extend(chunk)
    return bytes(result)


def frame_size(peer):
    value = 0
    for shift in range(0, 21, 7):
        byte = exact(peer, 1)[0]
        value |= (byte & 127) << shift
        if byte < 128:
            if value > 65536:
                raise RuntimeError("status probe frame exceeds limit")
            return value
    raise RuntimeError("invalid probe frame length")


def probe(port):
    with socket.create_connection(("127.0.0.1", port), timeout=1) as peer:
        peer.settimeout(1)
        hello = b"\x00" + varint(777) + b"\x09localhost" + port.to_bytes(2, "big") + b"\x01"
        peer.sendall(varint(len(hello)) + hello + b"\x01\x00")
        response = exact(peer, frame_size(peer))
        if not response or response[0] != 0:
            raise RuntimeError("invalid status probe response")


def exercise(binary, root, name, data, close_input=False, terminate=False):
    work = root / name
    config = work / "config"
    config.mkdir(parents=True)
    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        port = reserve.getsockname()[1]
    (config / "proxy.toml").write_text(
        f'[network]\nbind="127.0.0.1"\nport={port}\nonline-mode=false\ncompression-threshold=-1\n'
    )
    (config / "backends.toml").write_text(
        '[routing]\ndefault=["lobby"]\n[backends.lobby]\nhost="127.0.0.1"\nport=1\nforwarding="none"\n'
    )
    output = work / "output.log"
    with output.open("wb") as log:
        process = subprocess.Popen(
            [str(binary), "--config", str(config)], stdin=subprocess.PIPE,
            stdout=log, stderr=subprocess.STDOUT, cwd=work,
        )
        try:
            deadline = time.monotonic() + 8
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f"{name}: proxy exited during startup: {output.read_text()}")
                try:
                    probe(port)
                    break
                except (OSError, RuntimeError):
                    if time.monotonic() >= deadline:
                        raise RuntimeError(f"{name}: startup deadline expired")
                    time.sleep(0.02)
            process.stdin.write(data)
            process.stdin.flush()
            if close_input:
                process.stdin.close()
            if terminate:
                # A second status response demonstrates the listener remains active after input.
                probe(port)
                if process.poll() is not None:
                    raise RuntimeError(f"{name}: input unexpectedly stopped the proxy")
                process.send_signal(signal.SIGTERM)
            code = process.wait(timeout=4)
            if code:
                raise RuntimeError(f"{name}: exit {code}: {output.read_text()}")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=4)
            if not process.stdin.closed:
                process.stdin.close()
    return output.read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    build = pathlib.Path(__file__).resolve().parents[1] / "build"
    with tempfile.TemporaryDirectory(prefix="console-gate-", dir=build) as directory:
        root = pathlib.Path(directory)
        burst = exercise(binary, root, "burst", b"help\nservers\r\nstatus\nsend Nobody lobby\nstop\n")
        for expected in ("Commands:", "Backends: lobby", "Sessions:", "Player not found", "Proxy stopped."):
            if expected not in burst:
                raise RuntimeError(f"burst omitted {expected!r}: {burst}")
        overflow = exercise(binary, root, "overflow", b"x" * 4097 + b"\nhelp\nstop\n")
        if "line discarded" not in overflow or "Commands:" not in overflow:
            raise RuntimeError("oversized line corrupted the next command")
        partial = exercise(binary, root, "partial", b"send Nobody", terminate=True)
        if "Backend change preparation" in partial or "Player not found" in partial:
            raise RuntimeError("partial command executed before a delimiter")
        exercise(binary, root, "eof", b"", close_input=True, terminate=True)
        final = exercise(binary, root, "final", b"stop", close_input=True)
        if "Stopping proxy." not in final:
            raise RuntimeError("final unterminated command was lost")
    print(json.dumps({"passed": True, "cases": ["burst", "overflow", "partial-signal", "eof", "final-line"]}))


if __name__ == "__main__":
    main()
