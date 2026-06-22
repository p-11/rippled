#!/usr/bin/env python3
"""Open-loop, rate-ramp load driver (Ripple scalability-test methodology).

Submits at a fixed target rate from a random sender out of a fixed pool,
ramping level by level until a level fails either ledger-stability or
completion. Spreading each tick across the whole pool keeps every account far
below its queue/sequence limits, avoiding the per-account stalls a closed-loop
driver suffers at scale.
"""

from __future__ import annotations

import argparse
import json
import random
import threading
import time
from collections import Counter, defaultdict

import bench_lib as bl

try:
    from measure_disk import discover_containers, du_bytes
except Exception:  # measure_disk needs docker on the driver host; skip if absent
    discover_containers = du_bytes = None

ACCEPT = {"tesSUCCESS", "terQUEUED"}


class RateLimiter:
    def __init__(self, rate: float):
        self.interval = 1.0 / rate
        self.next = time.monotonic()
        self.lock = threading.Lock()

    def acquire(self) -> None:
        with self.lock:
            now = time.monotonic()
            if self.next < now:
                self.next = now
            wait = self.next - now
            self.next += self.interval
        if wait > 0:
            time.sleep(wait)


class LedgerGapMonitor(threading.Thread):
    def __init__(self, url: str, poll_s: float = 0.5):
        super().__init__(daemon=True)
        self.url = url
        self.poll_s = poll_s
        self._stop = threading.Event()
        self.lock = threading.Lock()
        self.last_idx = 0
        self.last_change = time.monotonic()
        self.max_gap = 0.0

    def reset(self) -> None:
        with self.lock:
            self.last_change = time.monotonic()
            self.max_gap = 0.0

    def current_gap(self) -> float:
        with self.lock:
            return time.monotonic() - self.last_change

    def stop(self) -> None:
        self._stop.set()

    def run(self) -> None:
        while not self._stop.is_set():
            try:
                idx = bl.current_ledger(self.url)
            except (bl.RpcError, OSError):
                idx = self.last_idx
            now = time.monotonic()
            with self.lock:
                if idx > self.last_idx:
                    self.last_idx = idx
                    self.last_change = now
                else:
                    self.max_gap = max(self.max_gap, now - self.last_change)
            time.sleep(self.poll_s)


def run_level(rate, hold, warmup, accounts, by_acct, cursor, acct_url, poller, gap_mon):
    """Submit at `rate` tx/s for `hold` s (after `warmup` s). Returns metrics."""
    limiter = RateLimiter(rate)
    workers = min(256, max(32, rate // 8))
    lock = threading.Lock()
    submitted: list = []
    engines: Counter = Counter()
    level_start = time.monotonic() + warmup
    stop_at = level_start + hold

    def worker() -> None:
        while time.monotonic() < stop_at:
            limiter.acquire()
            if time.monotonic() >= stop_at:
                return
            blob = None
            for _ in range(16):
                a = random.choice(accounts)
                with lock:
                    idx = cursor[a]
                    if idx < len(by_acct[a]):
                        cursor[a] = idx + 1
                        blob = by_acct[a][idx]["blob"]
                        break
            if blob is None:
                return  # pool exhausted
            t0 = time.monotonic()
            try:
                r = bl.rpc(acct_url[a], "submit", {"tx_blob": blob}, timeout=15)
                eng = r.get("engine_result")
                h = r.get("tx_json", {}).get("hash")
            except (bl.RpcError, OSError):
                eng, h = "rpc", None
            ok = bool(h) and eng in ACCEPT
            if ok:
                poller.track(h, t0)
            if t0 >= level_start:
                with lock:
                    submitted.append((h if ok else None, t0))
                    engines[eng or "rpc"] += 1

    gap_mon.reset()
    threads = [threading.Thread(target=worker, daemon=True) for _ in range(workers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # Grace period so in-flight transactions can validate before we score.
    time.sleep(12.0)

    lat = []
    completed = 0
    for h, t0 in submitted:
        if h is None:
            continue
        with poller._lock:
            done = poller.done.get(h)
        if done is not None:
            completed += 1
            lat.append((done[0] - t0) * 1000.0)
    n_sub = len(submitted)
    completion = completed / n_sub if n_sub else 0.0
    achieved = completed / hold
    # Corpus exhaustion: if we submitted well under the target rate AND the pool
    # is essentially drained, the shortfall is our pre-signed corpus running out,
    # not a network limit. Flag it so the caller doesn't read it as a knee.
    target = int(rate * hold)
    total_blobs = sum(len(v) for v in by_acct.values())
    consumed = sum(cursor.values())
    corpus_limited = n_sub < target * 0.9 and consumed >= total_blobs * 0.97
    return {
        "rate": rate,
        "submitted": n_sub,
        "target": target,
        "completed": completed,
        "completion": completion,
        "achieved_tps": achieved,
        "p50_ms": pct(lat, 50),
        "p95_ms": pct(lat, 95),
        "max_ledger_gap_s": gap_mon.max_gap,
        "corpus_limited": corpus_limited,
        "engines": dict(engines),
    }


def _wait_ready(url: str, ledgers: int = 5, timeout_s: float = 90.0) -> None:
    """Pause until the poller baseline is set and the network is closing ledgers
    at a steady cadence."""
    time.sleep(3.0)  # let LedgerPoller take its first sample
    try:
        start = bl.current_ledger(url)
    except (bl.RpcError, OSError):
        start = 0
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            if bl.current_ledger(url) >= start + ledgers:
                return
        except (bl.RpcError, OSError):
            pass
        time.sleep(0.5)


def pct(values: list, p: float) -> float:
    if not values:
        return float("nan")
    s = sorted(values)
    k = min(len(s) - 1, int(round(p / 100.0 * (len(s) - 1))))
    return round(s[k], 1)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--urls", default="http://127.0.0.1:5005")
    ap.add_argument("--corpus", default="corpus.jsonl")
    ap.add_argument("--out", default="run")
    ap.add_argument("--rate-start", type=int, default=500)
    ap.add_argument("--rate-step", type=int, default=250)
    ap.add_argument("--rate-cap", type=int, default=6000, help="safety ceiling")
    ap.add_argument("--rates", default="", help="explicit list; overrides start/step")
    ap.add_argument("--hold", type=float, default=30.0)
    ap.add_argument("--warmup", type=float, default=5.0)
    ap.add_argument("--ledger-max-gap", type=float, default=30.0)
    ap.add_argument("--completion-min", type=float, default=0.5)
    # Past saturation, excess load is absorbed as terQUEUED that still validates,
    # so completion stays high and never trips the abort. The real signal is that
    # achieved tps stops tracking the target; a level "tracks" if it achieves at
    # least this fraction of its target.
    ap.add_argument("--achieved-ratio-min", type=float, default=0.85)
    ap.add_argument("--disk-path", default="/var/lib/rippled/db")
    args = ap.parse_args()

    urls = [u for u in args.urls.split(",") if u]
    explicit = [int(x) for x in args.rates.split(",") if x] if args.rates else []

    by_acct: dict = defaultdict(list)
    with open(args.corpus, encoding="utf-8") as f:
        for line in f:
            e = json.loads(line)
            by_acct[e["account"]].append(e)
    for a in by_acct:
        by_acct[a].sort(key=lambda x: x["seq"])
    accounts = list(by_acct.keys())
    cursor = {a: 0 for a in accounts}
    acct_url = {a: urls[i % len(urls)] for i, a in enumerate(accounts)}

    plan = (
        f"explicit {explicit}"
        if explicit
        else f"open-ended {args.rate_start}+{args.rate_step} -> cap {args.rate_cap}"
    )
    print(
        f"{len(accounts)} accounts, rate plan: {plan} tx/s, hold={args.hold}s, "
        f"{len(urls)} endpoint(s); abort if ledger gap > {args.ledger_max_gap}s "
        f"or completion < {args.completion_min:.0%}",
        flush=True,
    )

    poller = bl.LedgerPoller(urls[0])
    poller.start()
    gap_mon = LedgerGapMonitor(urls[0])
    gap_mon.start()

    # Submitting into a cold poller/network produces a spurious low-completion
    # first level, and a traffic warmup just backlogs it, so we wait quietly.
    print("Settling (poller baseline + steady ledger cadence) ...", flush=True)
    _wait_ready(urls[0])

    def rate_sequence():
        if explicit:
            yield from explicit
            return
        r = args.rate_start
        while r <= args.rate_cap:
            yield r
            r += args.rate_step

    levels = []
    diverge_streak = 0
    for rate in rate_sequence():
        print(f"Rate level: {rate} tx/s ...", flush=True)
        row = run_level(
            rate,
            args.hold,
            args.warmup,
            accounts,
            by_acct,
            cursor,
            acct_url,
            poller,
            gap_mon,
        )
        # Corpus exhaustion also depresses achieved, so it is never read as a
        # plateau (treat corpus-limited levels as still tracking).
        tracking = (
            row["corpus_limited"]
            or row["achieved_tps"] >= args.achieved_ratio_min * rate
        )
        row["tracking"] = tracking
        diverge_streak = 0 if tracking else diverge_streak + 1
        fail = []
        if row["max_ledger_gap_s"] > args.ledger_max_gap:
            fail.append(f"ledger-gap {row['max_ledger_gap_s']:.0f}s")
        if row["completion"] < args.completion_min:
            fail.append(f"completion {row['completion']:.0%}")
        # Two consecutive non-tracking levels is the saturation knee. One level
        # alone could be run-to-run noise, so require the plateau to persist.
        if diverge_streak >= 2:
            fail.append(
                f"throughput plateau (achieved {row['achieved_tps']:.0f} "
                f"< {args.achieved_ratio_min:.0%} of {rate})"
            )
        row["aborted"] = bool(fail)
        levels.append(row)
        # Snapshot each node's on-disk size at this level boundary, while the
        # node is still healthy. Past the knee the overshoot can OOM the nodes;
        # the last healthy snapshot here yields storage growth through the knee
        # even when the final level crashes them. Best-effort (needs docker).
        if discover_containers is not None:
            try:
                row["disk_bytes"] = {
                    c: du_bytes(c, args.disk_path, attempts=1, timeout=10)
                    for c in discover_containers()
                }
            except Exception:
                row["disk_bytes"] = {}
        else:
            row["disk_bytes"] = {}
        tag = "ok"
        if row["corpus_limited"]:
            tag = "CORPUS DRY (raise --per-account; not a knee)"
        elif fail:
            tag = "ABORT: " + ", ".join(fail)
        eng_str = " ".join(
            f"{k}={v}"
            for k, v in sorted(row["engines"].items(), key=lambda x: -x[1])[:4]
        )
        print(
            f"  achieved={row['achieved_tps']:.1f} tps  completion={row['completion']:.0%}"
            f"  p50={row['p50_ms']}ms p95={row['p95_ms']}ms  max_gap={row['max_ledger_gap_s']:.0f}s"
            f"  [{eng_str}]  {tag}",
            flush=True,
        )
        # Stop on a real knee, or on corpus exhaustion (results past here are
        # invalid -- the corpus, not the network, is the limit).
        if fail or row["corpus_limited"]:
            break

    poller.stop()
    gap_mon.stop()

    # Max sustained rate is the highest offered rate still tracked; the headline
    # throughput is the max achieved tps across those levels.
    sustained = [
        r
        for r in levels
        if r.get("tracking") and not r["aborted"] and not r["corpus_limited"]
    ]
    max_sustained = max((r["rate"] for r in sustained), default=0)
    max_achieved = max((r["achieved_tps"] for r in sustained), default=0.0)
    corpus_capped = any(r["corpus_limited"] for r in levels)

    with open(f"{args.out}.cl_levels.csv", "w", encoding="utf-8") as f:
        f.write(
            "rate_tps,achieved_tps,submitted,completed,completion,"
            "p50_ms,p95_ms,max_ledger_gap_s,aborted,corpus_limited\n"
        )
        for r in levels:
            f.write(
                f"{r['rate']},{r['achieved_tps']:.1f},{r['submitted']},{r['completed']},"
                f"{r['completion']:.3f},{r['p50_ms']},{r['p95_ms']},"
                f"{r['max_ledger_gap_s']:.1f},{int(r['aborted'])},{int(r['corpus_limited'])}\n"
            )
    with open(f"{args.out}.cl_disk.json", "w", encoding="utf-8") as f:
        json.dump(
            [
                {
                    "rate": r["rate"],
                    "completed": r["completed"],
                    "disk_bytes": r.get("disk_bytes", {}),
                }
                for r in levels
            ],
            f,
            indent=2,
        )
    with open(f"{args.out}.cl_summary.json", "w", encoding="utf-8") as f:
        json.dump(
            {
                "max_sustained_rate_tps": max_sustained,
                "max_sustained_achieved_tps": round(max_achieved, 1),
                "corpus_capped": corpus_capped,
                "levels": levels,
            },
            f,
            indent=2,
        )
    print(
        f"\nSaturation knee: offered {max_sustained} tx/s, "
        f"sustained throughput {max_achieved:.1f} tps",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    import sys

    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print(f"\nFAILED: {e}", file=sys.stderr, flush=True)
        sys.exit(1)
