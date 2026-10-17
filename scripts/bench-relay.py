#!/usr/bin/env python3
"""Relay benchmark: N bots in play for a fixed window, per client version, through the proxy.

Reads the proxy's own relay byte counters from its console `status` before and after the window, and its CPU seconds and
peak RSS from /proc. The window starts after every bot has joined. Linux only, loopback, offline, one backend that
already listens on --backend-port. Result: --output/result.json.
"""
import argparse, json, os, platform, re, shutil, subprocess, sys, time
from pathlib import Path


def cpu_seconds(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def peak_rss_kb(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmHWM"):
            return int(line.split()[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--proxy", type=Path, required=True)
    parser.add_argument("--swarm", type=Path, required=True)
    parser.add_argument("--backend-port", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--count", type=int, default=50)
    parser.add_argument("--window", type=int, default=20)
    parser.add_argument("--versions", default="26.3,26.2,1.21.4,1.19.4")
    args = parser.parse_args()
    out = args.output.resolve()
    shutil.rmtree(out, ignore_errors=True)
    (out / "config").mkdir(parents=True)
    port = 49778
    (out / "config" / "proxy.toml").write_text(
        f'[network]\nbind = "127.0.0.1"\nport = {port}\nonline-mode = false\n'
        '[admission]\nglobal-rate = 100000\nglobal-burst = 100000\norigin-rate = 100000\norigin-burst = 100000\n'
        '[logging]\nfile = false\ncolor = "never"\n')
    (out / "config" / "backends.toml").write_text(
        '[routing]\ndefault = ["a"]\n[backends.a]\nhost = "127.0.0.1"\n'
        f'port = {args.backend_port}\nprotocol = 777\nforwarding = "none"\n')
    fifo = out / "console"
    os.mkfifo(fifo)
    holder = open(fifo, "r+b", buffering=0)
    log = (out / "proxy.log").open("wb")
    proxy = subprocess.Popen([str(args.proxy.resolve()), "--config", str(out / "config")], cwd=out,
                             stdin=open(fifo, "rb"), stdout=log, stderr=subprocess.STDOUT)
    time.sleep(1.5)

    def status():
        before = (out / "proxy.log").read_text(errors="replace").count("Sessions:")
        holder.write(b"status\n")
        for _ in range(100):
            text = (out / "proxy.log").read_text(errors="replace")
            if text.count("Sessions:") > before:
                break
            time.sleep(0.05)
        last = re.findall(r"Sessions: (\d+).*?toward clients: (\d+); toward backends: (\d+)", text)[-1]
        return tuple(int(v) for v in last)

    result = {"host": {"cpus": os.cpu_count(), "kernel": platform.release()}, "count": args.count,
              "window_seconds": args.window, "runs": []}
    try:
        for version in args.versions.split(","):
            bots = subprocess.Popen([str(args.swarm.resolve()), "--ip", f"127.0.0.1:{port}", "--count", str(args.count),
                                     "--delay", "20", "--version", version, "--no-physics"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(args.count * 0.02 + 8)
            sessions, c0, b0 = status()
            cpu0 = cpu_seconds(proxy.pid)
            start = time.monotonic()
            time.sleep(args.window)
            elapsed = time.monotonic() - start
            _, c1, b1 = status()
            cpu = cpu_seconds(proxy.pid) - cpu0
            bots.terminate()
            bots.wait()
            time.sleep(3)
            result["runs"].append({"client": version, "sessions": sessions,
                                   "toward_clients_kb_per_s": round((c1 - c0) / 1024 / elapsed, 1),
                                   "toward_backends_kb_per_s": round((b1 - b0) / 1024 / elapsed, 1),
                                   "proxy_cpu_percent_of_one_core": round(100 * cpu / elapsed, 1)})
        result["proxy_peak_rss_kb"] = peak_rss_kb(proxy.pid)
    finally:
        holder.write(b"stop\n")
        try:
            proxy.wait(5)
        except subprocess.TimeoutExpired:
            proxy.kill()
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


main()
