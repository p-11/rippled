#!/usr/bin/env python3
"""Per-PID resource monitor for the benchmark nodes."""

from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
import time

import bench_lib as bl

CLK_TCK = os.sysconf("SC_CLK_TCK")


def container_pid(name: str) -> int | None:
    try:
        out = subprocess.run(
            ["docker", "inspect", "-f", "{{.State.Pid}}", name],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
        pid = int(out)
        return pid if pid > 0 else None
    except (subprocess.CalledProcessError, ValueError):
        return None


def read_cpu_ticks(pid: int) -> int | None:
    try:
        with open(f"/proc/{pid}/stat", encoding="utf-8") as f:
            fields = f.read().split()
        # Fields 14 (utime) and 15 (stime), 1-indexed; account for a comm with
        # spaces by splitting after the trailing ')'.
        return int(fields[13]) + int(fields[14])
    except (OSError, IndexError, ValueError):
        return None


def read_rss_mb(pid: int) -> float | None:
    try:
        with open(f"/proc/{pid}/status", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) / 1024.0  # KiB -> MiB
    except OSError:
        return None
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--containers",
        default="rippled-validator-1,rippled-validator-2,rippled-validator-3,"
        "rippled-validator-4,rippled-validator-5,rippled-stock-1,rippled-stock-2,rippled-stock-3",
    )
    ap.add_argument(
        "--url", default="http://127.0.0.1:5005", help="RPC node for JobQueue depth"
    )
    ap.add_argument("--interval", type=float, default=0.1)
    ap.add_argument("--duration", type=float, default=120.0)
    ap.add_argument("--out", default="monitor.csv")
    args = ap.parse_args()

    names = [c for c in args.containers.split(",") if c]
    pids = {n: container_pid(n) for n in names}
    pids = {n: p for n, p in pids.items() if p}
    if not pids:
        print("No container PIDs resolved; is the DevNet up?", file=sys.stderr)
        return 1
    print(
        f"Monitoring {list(pids)} for {args.duration}s @ {args.interval*1000:.0f}ms",
        flush=True,
    )

    last_ticks: dict[str, int] = {}
    last_t = time.monotonic()
    rows: list[dict] = []
    end = time.monotonic() + args.duration
    while time.monotonic() < end:
        t = time.monotonic()
        dt = t - last_t
        backlog = bl.job_backlog(args.url)
        for name, pid in pids.items():
            ticks = read_cpu_ticks(pid)
            rss = read_rss_mb(pid)
            cpu_pct = float("nan")
            if ticks is not None and name in last_ticks and dt > 0:
                cpu_pct = (ticks - last_ticks[name]) / (dt * CLK_TCK) * 100.0
            if ticks is not None:
                last_ticks[name] = ticks
            rows.append(
                {
                    "time": f"{t:.3f}",
                    "node": name,
                    "cpu_pct": f"{cpu_pct:.1f}",
                    "rss_mb": f"{rss:.1f}" if rss is not None else "",
                    "job_backlog": backlog,
                }
            )
        last_t = t
        sleep = args.interval - (time.monotonic() - t)
        if sleep > 0:
            time.sleep(sleep)

    with open(args.out, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(
            f, fieldnames=["time", "node", "cpu_pct", "rss_mb", "job_backlog"]
        )
        w.writeheader()
        w.writerows(rows)
    print(f"Wrote {len(rows)} samples to {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print(f"\nFAILED: {e}", file=sys.stderr, flush=True)
        sys.exit(1)
