#!/usr/bin/env python3
"""Score a steady_state_driver.py run from its raw files.

Everything is recomputed from what was recorded (schedule, every submit, every
validated ledger and its transactions, validator and fee/queue samples,
generator CPU); the driver's online verdicts are not trusted, only compared.

Per load level, over its steady-state measurement window only:
  ledger side   validated-ledger intervals (max / p95 / mean), ledgers closed,
                transactions per ledger, throughput = transactions in ledgers
                validated inside the window / window length
  generator     target vs sent, send lag vs schedule, submit RPC latency,
                generator CPU, every engine result counted separately
                (terQUEUED is never folded into success)
  inclusion     share of the window's submitted transactions validated inside
                the window, and inside the window + drain (both reported)
  validators    consensus round time (converge_time_s), job queue backlog,
                dropped-transaction counters, load factor
  queue         expected ledger size, open-ledger size, queue size, open-ledger fee

A level passes when every validated-ledger interval in the window is at most
max_interval, AND the generator delivered the target (otherwise the level is
"generator-limited" and says nothing about the network). Capacity is the highest
level such that it and every level below it passed.

Usage: analyze_steady_state.py PREFIX   (writes PREFIX.summary.json, PREFIX.levels.csv)
"""

from __future__ import annotations

import csv
import glob
import gzip
import json
import math
import sys
from collections import Counter, defaultdict

import bench_lib as bl

PHASE_MEASURE = 2


def pct(xs, p):
    v = bl.percentile(xs, p)
    return None if (isinstance(v, float) and math.isnan(v)) else round(v, 3)


def read_jsonl(path):
    try:
        with open(path, encoding="utf-8") as f:
            return [json.loads(line) for line in f if line.strip()]
    except FileNotFoundError:
        return []


def ledger_window(ledgers, t0, t1):
    """Same definition as ValidatedLedgerRecorder.window, from the jsonl rows."""
    seen = sorted(
        (r["first_seen"], r["ledger_index"], r.get("txn_count"))
        for r in ledgers
        if r.get("first_seen") is not None and not math.isnan(r["first_seen"])
    )
    before = [t for t, _, _ in seen if t < t0]
    inside = [(t, li, c) for t, li, c in seen if t0 <= t <= t1]
    times = ([before[-1]] if before else []) + [t for t, _, _ in inside]
    intervals = [b - a for a, b in zip(times, times[1:]) if b > a]
    open_gap = t1 - times[-1] if times else t1 - t0
    counts = [c for _, _, c in inside if c is not None]
    return {
        "ledgers": len(inside),
        "first_index": inside[0][1] if inside else None,
        "last_index": inside[-1][1] if inside else None,
        "max_interval_s": round(max(intervals + [open_gap]), 3),
        "p95_interval_s": pct(intervals, 95),
        "mean_interval_s": round(sum(intervals) / len(intervals), 3) if intervals else None,
        "open_gap_s": round(open_gap, 3),
        "txn_total": sum(counts),
        "txn_per_ledger_mean": round(sum(counts) / len(counts), 1) if counts else 0.0,
        "txn_per_ledger_min": min(counts) if counts else None,
        "txn_per_ledger_max": max(counts) if counts else None,
        "missing_details": len(inside) - len(counts),
    }


def load_submits(prefix):
    rows = []
    for path in sorted(glob.glob(f"{prefix}.submits.p*.csv.gz")):
        with gzip.open(path, "rt", newline="") as f:
            for r in csv.DictReader(f):
                rows.append(
                    (
                        float(r["deadline"]),
                        float(r["sent"]),
                        float(r["done"]),
                        int(r["level"]),
                        int(r["phase"]),
                        r["engine"],
                        r["hash"],
                    )
                )
    return rows


def load_inclusions(prefix, wanted, ledger_seen):
    """hash -> first_seen time of the ledger that validated it, for wanted hashes."""
    out = {}
    path = f"{prefix}.ledger_txs.csv.gz"
    try:
        with gzip.open(path, "rt") as f:
            next(f, None)
            for line in f:
                li, h = line.rstrip("\n").split(",", 1)
                if h in wanted:
                    out[h] = ledger_seen.get(int(li))
    except FileNotFoundError:
        pass
    return out


def window_samples(samples, t0, t1):
    by = defaultdict(list)
    for s in samples:
        if t0 <= s["t"] <= t1:
            by[s["node"]].append(s)
    return by


def delta(rows, key):
    vals = [r.get(key) for r in rows if r.get(key) is not None]
    return (vals[-1] - vals[0]) if len(vals) >= 2 else None


def validator_stats(by_node):
    out = {}
    for node, rows in sorted(by_node.items()):
        # one converge time per closed ledger
        per_ledger = {}
        for r in rows:
            if r.get("validated_seq") is not None and r.get("converge_time_s") is not None:
                per_ledger[r["validated_seq"]] = r["converge_time_s"]
        conv = list(per_ledger.values())
        backlog = [r["job_backlog"] for r in rows if r.get("job_backlog") is not None]
        lf = [r["load_factor"] for r in rows if isinstance(r.get("load_factor"), (int, float))]
        states = Counter(r.get("server_state") for r in rows)
        out[node] = {
            "samples": len(rows),
            "converge_time_max_s": max(conv) if conv else None,
            "converge_time_p95_s": pct(conv, 95),
            "job_backlog_max": max(backlog) if backlog else None,
            "job_backlog_p95": pct(backlog, 95),
            "jq_trans_overflow_delta": delta(rows, "jq_trans_overflow"),
            "peer_disconnects_resources_delta": delta(rows, "peer_disconnects_resources"),
            "load_factor_max": max(lf) if lf else None,
            "server_states": dict(states),
        }
    return out


def fee_stats(by_node):
    out = {}
    for node, rows in sorted(by_node.items()):
        def col(k):
            return [r[k] for r in rows if r.get(k) is not None]

        out[node] = {
            "expected_ledger_size_min": min(col("expected_ledger_size"), default=None),
            "expected_ledger_size_max": max(col("expected_ledger_size"), default=None),
            "current_ledger_size_max": max(col("current_ledger_size"), default=None),
            "current_queue_size_max": max(col("current_queue_size"), default=None),
            "max_queue_size": max(col("max_queue_size"), default=None),
            "open_ledger_fee_max": max(col("open_ledger_fee"), default=None),
        }
    return out


def gen_cpu_stats(prefix, t0, t1):
    per_proc = defaultdict(list)
    try:
        with open(f"{prefix}.gen_cpu.csv") as f:
            for r in csv.DictReader(f):
                t = float(r["t"])
                if t0 <= t <= t1:
                    per_proc[r["proc"]].append(float(r["cpu_pct"]))
    except FileNotFoundError:
        return {}
    if not per_proc:
        return {}
    return {
        "per_proc_max_pct": max(max(v) for v in per_proc.values()),
        "per_proc_mean_pct": round(
            sum(sum(v) / len(v) for v in per_proc.values()) / len(per_proc), 1
        ),
        "procs": len(per_proc),
    }


def analyze(prefix: str) -> dict:
    with open(f"{prefix}.schedule.json") as f:
        sched = json.load(f)
    cfg = sched["config"]
    max_interval = cfg["max_interval"]
    drain = cfg["drain"]
    measure = cfg["measure"]
    gen_delivery_min = cfg["gen_delivery_min"]
    gen_late_max = cfg["gen_late_max"]
    gen_lag_max = cfg["gen_lag_max"]

    ledgers = read_jsonl(f"{prefix}.ledgers.jsonl")
    ledger_seen = {r["ledger_index"]: r.get("first_seen") for r in ledgers}
    sinfo = read_jsonl(f"{prefix}.server_info.jsonl")
    fees = read_jsonl(f"{prefix}.fee.jsonl")
    submits = load_submits(prefix)

    by_level = defaultdict(list)
    for s in submits:
        if s[4] == PHASE_MEASURE and s[3] >= 0:
            by_level[s[3]].append(s)
    wanted = {s[6] for lv in by_level.values() for s in lv if s[6]}
    incl = load_inclusions(prefix, wanted, ledger_seen)

    try:
        with open(f"{prefix}.online.json") as f:
            online = {r["level"]: r for r in json.load(f)["levels"]}
    except FileNotFoundError:
        online = {}

    last_ledger_t = max((r["first_seen"] for r in ledgers if r.get("first_seen")), default=0)
    levels = []
    for lv in sched["levels"]:
        i = lv["level"]
        m0, m1 = lv["measure"]
        if last_ledger_t < m1 and i not in online:
            continue  # the run stopped before this level's window
        rows = by_level.get(i, [])
        target = lv["rate"] * measure
        sent = [r for r in rows if r[5] != "exhausted"]
        lags = [r[1] - r[0] for r in sent]
        rpc_lat = [(r[2] - r[1]) * 1000.0 for r in sent]
        engines = Counter(r[5] for r in rows)
        hashed = [r for r in sent if r[6]]
        in_win = in_drain = 0
        conf_lat = []
        for r in hashed:
            t = incl.get(r[6])
            if t is None:
                continue
            if t <= m1:
                in_win += 1
            if t <= m1 + drain:
                in_drain += 1
            conf_lat.append((t - r[1]) * 1000.0)
        late = sum(1 for x in lags if x > gen_lag_max)
        delivery = len(sent) / target if target else 0.0
        late_frac = late / len(sent) if sent else 1.0
        exhausted = engines.get("exhausted", 0) > 0
        gen_ok = delivery >= gen_delivery_min and late_frac <= gen_late_max and not exhausted

        lw = ledger_window(ledgers, m0, m1)
        net_ok = lw["ledgers"] > 0 and lw["max_interval_s"] <= max_interval
        vs = window_samples([s for s in sinfo if s["node"].startswith("validator")], m0, m1)
        ss = window_samples([s for s in sinfo if s["node"].startswith("stock")], m0, m1)
        fs = window_samples(fees, m0, m1)

        levels.append(
            {
                "level": i,
                "rate": lv["rate"],
                "window": [round(m0, 3), round(m1, 3)],
                "verdict": ("PASS" if net_ok else "FAIL") if gen_ok else "GENERATOR-LIMITED",
                "network_pass": net_ok,
                "generator_ok": gen_ok,
                "ledger": {**lw, "ledger_tps": round(lw["txn_total"] / measure, 1)},
                "generator": {
                    "target": target,
                    "sent": len(sent),
                    "delivery": round(delivery, 4),
                    "send_lag_p50_s": pct(lags, 50),
                    "send_lag_p99_s": pct(lags, 99),
                    "send_lag_max_s": round(max(lags), 3) if lags else None,
                    "late_frac": round(late_frac, 4),
                    "submit_rpc_ms_p50": pct(rpc_lat, 50),
                    "submit_rpc_ms_p99": pct(rpc_lat, 99),
                    "submit_rpc_ms_max": round(max(rpc_lat), 1) if rpc_lat else None,
                    "cpu": gen_cpu_stats(prefix, m0, m1),
                },
                "engine_results": dict(engines.most_common()),
                "inclusion": {
                    "submitted_with_hash": len(hashed),
                    "validated_in_window": in_win,
                    "validated_in_window_frac": round(in_win / len(hashed), 4) if hashed else None,
                    "validated_with_drain": in_drain,
                    "validated_with_drain_frac": round(in_drain / len(hashed), 4) if hashed else None,
                    "drain_s": drain,
                    "confirm_ms_p50": pct(conf_lat, 50),
                    "confirm_ms_p95": pct(conf_lat, 95),
                },
                "validators": validator_stats(vs),
                "stock_nodes": validator_stats(ss),
                "queue": fee_stats(fs),
                "online": online.get(i),
            }
        )

    capacity = None
    for row in levels:
        if row["network_pass"] and row["generator_ok"]:
            capacity = row
        else:
            break
    summary = {
        "prefix": prefix,
        "criterion": f"every validated-ledger interval <= {max_interval}s over a "
        f"{measure:.0f}s steady-state window",
        "capacity": None
        if capacity is None
        else {
            "rate": capacity["rate"],
            "ledger_tps": capacity["ledger"]["ledger_tps"],
            "txn_per_ledger_mean": capacity["ledger"]["txn_per_ledger_mean"],
            "max_interval_s": capacity["ledger"]["max_interval_s"],
        },
        "first_failure": next(
            ({"rate": r["rate"], "verdict": r["verdict"]} for r in levels if r["verdict"] != "PASS"),
            None,
        ),
        "rate_step": (sched["levels"][1]["rate"] - sched["levels"][0]["rate"])
        if len(sched["levels"]) > 1
        else None,
        "stop_reason": (json.load(open(f"{prefix}.online.json")) or {}).get("stop_reason")
        if online
        else None,
        "levels": levels,
    }
    with open(f"{prefix}.summary.json", "w") as f:
        json.dump(summary, f, indent=2)

    cols = [
        "rate", "verdict", "max_interval_s", "p95_interval_s", "ledgers", "txn_per_ledger_mean",
        "ledger_tps", "sent", "target", "delivery", "send_lag_p99_s", "submit_rpc_ms_p99",
        "gen_cpu_max_pct", "tesSUCCESS", "terQUEUED", "other_results", "validated_in_window_frac",
        "validated_with_drain_frac", "val_converge_max_s", "val_backlog_max", "val_jq_overflow",
        "expected_ledger_size_max", "queue_size_max",
    ]
    with open(f"{prefix}.levels.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(cols)
        for r in levels:
            eng = r["engine_results"]
            vals = r["validators"].values()

            def vmax(k):
                xs = [v[k] for v in vals if v.get(k) is not None]
                return max(xs) if xs else ""

            q = r["queue"].values()
            w.writerow(
                [
                    r["rate"], r["verdict"], r["ledger"]["max_interval_s"],
                    r["ledger"]["p95_interval_s"], r["ledger"]["ledgers"],
                    r["ledger"]["txn_per_ledger_mean"], r["ledger"]["ledger_tps"],
                    r["generator"]["sent"], r["generator"]["target"], r["generator"]["delivery"],
                    r["generator"]["send_lag_p99_s"], r["generator"]["submit_rpc_ms_p99"],
                    r["generator"]["cpu"].get("per_proc_max_pct", ""),
                    eng.get("tesSUCCESS", 0), eng.get("terQUEUED", 0),
                    sum(v for k, v in eng.items() if k not in ("tesSUCCESS", "terQUEUED")),
                    r["inclusion"]["validated_in_window_frac"],
                    r["inclusion"]["validated_with_drain_frac"],
                    vmax("converge_time_max_s"), vmax("job_backlog_max"),
                    vmax("jq_trans_overflow_delta"),
                    max((x["expected_ledger_size_max"] or 0 for x in q), default=""),
                    max((x["current_queue_size_max"] or 0 for x in q), default=""),
                ]
            )
    return summary


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    s = analyze(sys.argv[1])
    for r in s["levels"]:
        print(
            f"{r['rate']:>6} tx/s  {r['verdict']:<18} max_int={r['ledger']['max_interval_s']}s "
            f"tx/ledger={r['ledger']['txn_per_ledger_mean']} ledger_tps={r['ledger']['ledger_tps']} "
            f"delivery={r['generator']['delivery']}"
        )
    print("capacity:", json.dumps(s["capacity"]))
