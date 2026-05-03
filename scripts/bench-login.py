#!/usr/bin/env python3
"""Login benchmark: N bots straight to a backend, then through the proxy.

Measures wall time until the backend logs N joins, and for the proxy run its CPU seconds (utime+stime from /proc)
and peak RSS. The backend is whatever already listens on --backend-port; its cost is common to both runs.
Linux only, loopback only, offline mode. Results go to --output/result.json together with host and version facts.
"""
import argparse, json, os, platform, re, shutil, subprocess, sys, time
from pathlib import Path


def cpu_seconds(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def rss_peak_kb(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmHWM"):
            return int(line.split()[1])


def joins(log):
    return len(re.findall(r"\[Server\]: Bot_\d+ joined the game", log.read_text(errors="replace")))


def run_bots(swarm, address, count, delay, backend_log, baseline, timeout):
    start = time.monotonic()
    bots = subprocess.Popen([swarm, "--ip", address, "--count", str(count), "--delay", str(delay), "--version", "26.3",
                             "--no-physics"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    while joins(backend_log) - baseline < count and time.monotonic() - start < timeout:
        time.sleep(0.02)
    elapsed = time.monotonic() - start
    reached = joins(backend_log) - baseline
    return bots, elapsed, reached


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--proxy", type=Path, required=True)
    parser.add_argument("--swarm", type=Path, required=True)
    parser.add_argument("--backend-port", type=int, required=True)
    parser.add_argument("--backend-log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--delay", type=int, default=10)
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    out = args.output.resolve()
    shutil.rmtree(out, ignore_errors=True)
    (out / "config").mkdir(parents=True)
    port = 49777
    (out / "config" / "proxy.toml").write_text(
        f'[network]\nbind = "127.0.0.1"\nport = {port}\nonline-mode = false\n'
        '[admission]\nglobal-rate = 100000\nglobal-burst = 100000\norigin-rate = 100000\norigin-burst = 100000\n'
        '[logging]\nfile = false\n')
    (out / "config" / "backends.toml").write_text(
        '[routing]\ndefault = ["a"]\n[backends.a]\nhost = "127.0.0.1"\n'
        f'port = {args.backend_port}\nprotocol = 777\nforwarding = "none"\n')
    proxy = subprocess.Popen([str(args.proxy.resolve()), "--config", str(out / "config")], cwd=out,
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    result = {"host": {"cpus": os.cpu_count(), "kernel": platform.release(), "python": platform.python_version()},
              "count": args.count, "delay_ms": args.delay, "rounds": []}
    try:
        for label, address in (("direct", f"127.0.0.1:{args.backend_port}"), ("proxy", f"127.0.0.1:{port}")):
            for round_number in range(args.rounds):
                baseline = joins(args.backend_log)
                cpu0 = cpu_seconds(proxy.pid)
                bots, elapsed, reached = run_bots(str(args.swarm.resolve()), address, args.count, args.delay,
                                                  args.backend_log, baseline, 60)
                cpu = cpu_seconds(proxy.pid) - cpu0
                bots.terminate()
                bots.wait()
                time.sleep(3)
                result["rounds"].append({"path": label, "round": round_number + 1, "joined": reached,
                                         "seconds": round(elapsed, 3),
                                         "proxy_cpu_seconds": round(cpu, 3) if label == "proxy" else None})
        result["proxy_peak_rss_kb"] = rss_peak_kb(proxy.pid)
    finally:
        proxy.terminate()
        proxy.wait()
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


main()
