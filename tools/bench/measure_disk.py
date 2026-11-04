#!/usr/bin/env python3
"""Snapshot each DevNet node's on-disk database size.

What a node stores per transaction (tx + metadata + SHAMap/nodestore) is larger
than the wire signature and can only be measured, not derived. Snapshot before
and after the timed run; the difference is the storage the run added.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time

DB_PATH = "/var/lib/rippled/db"


def discover_containers() -> list[str]:
    out = subprocess.run(
        ["docker", "ps", "--format", "{{.Names}}"],
        capture_output=True,
        text=True,
    ).stdout
    return sorted(n for n in out.split() if n.startswith("rippled-"))


def du_bytes(container: str, path: str, attempts: int = 5, timeout: int = 60) -> int:
    """Apparent byte size of `path` inside `container`; -1 if unavailable.

    Retries with a fixed backoff: right after a saturation collapse the node is
    briefly unresponsive and `du` times out under the write backlog, but it
    recovers once the backlog drains. A healthy node answers on the first try,
    so the passing runs pay no extra time.
    """
    for i in range(attempts):
        try:
            r = subprocess.run(
                ["docker", "exec", container, "du", "-sb", path],
                capture_output=True,
                text=True,
                timeout=timeout,
            )
            if r.returncode == 0:
                return int(r.stdout.split()[0])
        except (OSError, ValueError, IndexError, subprocess.TimeoutExpired):
            pass
        if i < attempts - 1:
            time.sleep(15)
    return -1


def du_components(container: str, path: str = DB_PATH, timeout: int = 60) -> dict:
    """Per-component sizes inside the database directory: the NuDB node store
    and each SQLite file (with its WAL), so growth can be attributed to the node
    store vs. the transaction / ledger databases. {} if unavailable."""
    script = (
        f"cd {path} 2>/dev/null || exit 1; "
        "for p in nudb *.db *.db-wal *.db-shm; do [ -e \"$p\" ] && du -sb \"$p\"; done; true"
    )
    try:
        r = subprocess.run(
            ["docker", "exec", container, "sh", "-c", script],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired):
        return {}
    if r.returncode != 0:
        return {}
    out = {}
    for line in r.stdout.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[0].isdigit():
            out[parts[1].strip()] = int(parts[0])
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--containers",
        default=None,
        help="comma-separated container names (default: auto-detect rippled-*)",
    )
    ap.add_argument("--path", default=DB_PATH)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    containers = (
        [c for c in args.containers.split(",") if c]
        if args.containers
        else discover_containers()
    )
    sizes = {c: du_bytes(c, args.path) for c in containers}
    components = {c: du_components(c, args.path) for c in containers}
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(
            {
                "t": time.time(),
                "path": args.path,
                "sizes_bytes": sizes,
                "components_bytes": components,
            },
            f,
            indent=2,
        )
    summary = ", ".join(f"{c}={b}" for c, b in sizes.items())
    print(f"disk snapshot ({args.path}): {summary}")
    # Return non-zero on an empty/all-failed snapshot so the caller does not record
    # it as a successful measurement. A hybrid run once produced no storage numbers
    # unnoticed: the nodes had exited by snapshot time and the empty result was
    # swallowed.
    if not sizes:
        print(
            "measure_disk: no running rippled-* containers found at snapshot time",
            file=sys.stderr,
        )
        return 2
    if all(b < 0 for b in sizes.values()):
        print("measure_disk: every container size probe failed", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print(f"\nFAILED: {e}", file=sys.stderr, flush=True)
        sys.exit(1)
