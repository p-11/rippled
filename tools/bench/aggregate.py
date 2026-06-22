#!/usr/bin/env python3
"""Aggregate a benchmark run into report tables.

Consumes a run's artifact families (PerfLog files, the resource monitor CSV,
the rate-ramp driver outputs, and an optional microbench JSON) and emits a
single Markdown report.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import statistics
import sys


def percentile(values: list[float], p: float) -> float:
    if not values:
        return float("nan")
    s = sorted(values)
    k = min(len(s) - 1, int(round(p / 100.0 * (len(s) - 1))))
    return s[k]


def bootstrap_median_ci(
    values: list[float], iters: int = 2000, seed: int = 12345, cap: int = 40000
) -> tuple[float, float]:
    """Bootstrap 95% confidence interval on the median.

    Seeded so the report is reproducible.

    Large samples are subsampled to `cap` first: the bootstrap is O(iters * n)
    and high-frequency probes can emit millions of events, but the CI on the
    median is already tight at `cap`, so the result is unchanged to the
    reported precision.
    """
    import random

    if len(values) < 2:
        return (float("nan"), float("nan"))
    rng = random.Random(seed)
    if len(values) > cap:
        values = [values[rng.randrange(len(values))] for _ in range(cap)]
    n = len(values)
    meds = []
    for _ in range(iters):
        sample = [values[rng.randrange(n)] for _ in range(n)]
        meds.append(statistics.median(sample))
    meds.sort()
    return (percentile(meds, 2.5), percentile(meds, 97.5))


def parse_perf_dir(perf_dir: str) -> dict[str, dict[str, list[float]]]:
    """node -> tag -> list of durations (us)."""
    out: dict[str, dict[str, list[float]]] = {}
    for path in glob.glob(os.path.join(perf_dir, "*", "perf.log")):
        node = os.path.basename(os.path.dirname(path))
        tags: dict[str, list[float]] = {}
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or '"event"' not in line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    continue
                tag = rec.get("event")
                dur = rec.get("duration_us")
                if tag is not None and dur is not None:
                    tags.setdefault(tag, []).append(float(dur))
        if tags:
            out[node] = tags
    return out


def parse_monitor(path: str) -> dict[str, dict[str, list[float]]]:
    """node -> {cpu_pct: [...], rss_mb: [...], job_backlog: [...]}."""
    import csv

    out: dict[str, dict[str, list[float]]] = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            node = row["node"]
            d = out.setdefault(node, {"cpu_pct": [], "rss_mb": [], "job_backlog": []})
            for key in ("cpu_pct", "rss_mb", "job_backlog"):
                v = row.get(key, "")
                if v not in ("", "nan"):
                    try:
                        d[key].append(float(v))
                    except ValueError:
                        pass
    return out


def md_table(headers: list[str], rows: list[list[str]]) -> str:
    out = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for r in rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(out)


def verify_overhead_table(perf: dict[str, dict[str, list[float]]]) -> str:
    by_tag: dict[str, list[float]] = {}
    for tags in perf.values():
        for tag, durs in tags.items():
            by_tag.setdefault(tag, []).extend(durs)
    rows = []
    for tag in sorted(by_tag):
        d = by_tag[tag]
        lo, hi = bootstrap_median_ci(d)
        rows.append(
            [
                tag,
                len(d),
                f"{statistics.median(d):.0f}",
                f"{lo:.0f}-{hi:.0f}",
                f"{percentile(d, 95):.0f}",
                f"{percentile(d, 99):.0f}",
            ]
        )
    return md_table(
        ["probe", "n", "median_us", "median_95ci_us", "p95_us", "p99_us"], rows
    )


def describe(values: list[float]) -> dict[str, float]:
    """Full-distribution summary matching pandas DataFrame.describe(), plus tails.

    Uses the sample standard deviation (ddof=1) so numbers line up with a
    pandas describe() on the same data.
    """
    s = sorted(values)
    n = len(s)
    return {
        "count": n,
        "mean": statistics.fmean(s),
        "std": statistics.stdev(s) if n > 1 else 0.0,
        "min": s[0],
        "p25": percentile(s, 25),
        "p50": percentile(s, 50),
        "p75": percentile(s, 75),
        "p95": percentile(s, 95),
        "p99": percentile(s, 99),
        "max": s[-1],
    }


def distribution_table(perf: dict[str, dict[str, list[float]]]) -> str:
    """Per-probe full distribution (us per probe), pooled across all nodes."""
    by_tag: dict[str, list[float]] = {}
    for tags in perf.values():
        for tag, durs in tags.items():
            by_tag.setdefault(tag, []).extend(durs)
    cols = ["min", "p25", "p50", "p75", "p95", "p99", "max"]
    rows = []
    for tag in sorted(by_tag):
        d = describe(by_tag[tag])
        rows.append(
            [tag, d["count"], f"{d['mean']:.0f}", f"{d['std']:.0f}"]
            + [f"{d[c]:.0f}" for c in cols]
        )
    return md_table(["probe", "n", "mean", "std"] + cols, rows)


def disk_growth_table(before: dict, after: dict, validated_total: int) -> str | None:
    """On-disk database growth per node over the timed run.

    bytes_per_tx is the on-disk cost (tx + metadata + SHAMap/nodestore
    overhead), not just the wire signature size.
    """
    b = before.get("sizes_bytes", {})
    a = after.get("sizes_bytes", {})
    nodes = sorted(set(b) & set(a))
    rows = []
    deltas = []
    for node in nodes:
        if b[node] < 0 or a[node] < 0:
            continue
        grown = a[node] - b[node]
        deltas.append(grown)
        per_tx = grown / validated_total if validated_total else float("nan")
        rows.append(
            [
                node,
                f"{b[node] / 1e6:.1f}",
                f"{a[node] / 1e6:.1f}",
                f"{grown / 1e6:.1f}",
                f"{per_tx:.0f}" if validated_total else "-",
            ]
        )
    if not rows:
        return None
    table = md_table(
        ["node", "before_MB", "after_MB", "grown_MB", "bytes_per_tx"], rows
    )
    median_per_tx = (
        statistics.median(deltas) / validated_total if validated_total else float("nan")
    )
    summary = (
        f"\n- Median on-disk growth per validated transaction: "
        f"**{median_per_tx:.0f} bytes** over {validated_total} transactions"
        if validated_total
        else ""
    )
    return table + summary


def resource_table(monitor: dict[str, dict[str, list[float]]]) -> str:
    rows = []
    for node in sorted(monitor):
        m = monitor[node]
        cpu = m["cpu_pct"]
        rss = m["rss_mb"]
        bl = m["job_backlog"]
        rows.append(
            [
                node,
                f"{statistics.median(cpu):.0f}" if cpu else "-",
                f"{max(cpu):.0f}" if cpu else "-",
                f"{statistics.median(rss):.0f}" if rss else "-",
                f"{max(rss):.0f}" if rss else "-",
                f"{max(bl):.0f}" if bl else "-",
            ]
        )
    return md_table(
        [
            "node",
            "cpu_median_%",
            "cpu_max_%",
            "rss_median_MiB",
            "rss_max_MiB",
            "backlog_max",
        ],
        rows,
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--perf-dir", default="scripts/devnet/perf")
    ap.add_argument("--monitor", default="monitor.csv")
    ap.add_argument("--driver-prefix", default="run")
    ap.add_argument("--microbench", default=None)
    ap.add_argument("--disk-before", default=None)
    ap.add_argument("--disk-after", default=None)
    ap.add_argument("--label", default="run")
    ap.add_argument("--out", default="report.md")
    args = ap.parse_args()

    # Disk snapshots default to sitting next to the driver outputs.
    out_dir = os.path.dirname(args.driver_prefix)
    disk_before_path = args.disk_before or os.path.join(out_dir, "disk_before.json")
    disk_after_path = args.disk_after or os.path.join(out_dir, "disk_after.json")

    perf = parse_perf_dir(args.perf_dir)
    monitor = parse_monitor(args.monitor)

    parts = [f"# Benchmark report: {args.label}\n"]

    cl_path = f"{args.driver_prefix}.cl_summary.json"
    if os.path.exists(cl_path):
        with open(cl_path, encoding="utf-8") as f:
            cl = json.load(f)
        cl_levels = cl.get("levels", [])
        parts.append("## Throughput (open-loop rate-ramp, saturation knee)\n")
        parts.append(
            f"- Max sustained: **{cl.get('max_sustained_achieved_tps', float('nan'))} tps** "
            f"delivered (knee onset at offered rate "
            f"{cl.get('max_sustained_rate_tps', '-')} tx/s)"
        )
        if cl.get("corpus_capped"):
            parts.append(
                "- _A level hit pre-signed-corpus exhaustion; corpus-capped "
                "levels are excluded from the sustained figure._"
            )
        if cl_levels:
            parts.append("")
            parts.append(
                md_table(
                    [
                        "rate_tps",
                        "achieved_tps",
                        "completion",
                        "p50_ms",
                        "p95_ms",
                        "max_gap_s",
                        "tracking",
                    ],
                    [
                        [
                            lv["rate"],
                            round(lv["achieved_tps"], 1),
                            f"{lv['completion']:.0%}",
                            lv["p50_ms"],
                            lv["p95_ms"],
                            lv["max_ledger_gap_s"],
                            "yes" if lv.get("tracking") else "no",
                        ]
                        for lv in cl_levels
                    ],
                )
            )
            parts.append(
                "\n_A level `tracks` when achieved tps holds within 85% of the "
                "offered rate. The knee is where tracking is lost: the network "
                "is saturated and excess load queues instead of raising "
                "throughput. Sustained throughput is the delivered (achieved) "
                "tps, not the offered rate._"
            )
        validated_key = "completed"

        disk_before = None
        if os.path.exists(disk_before_path):
            with open(disk_before_path, encoding="utf-8") as f:
                disk_before = json.load(f)
        base_nodes = set((disk_before or {}).get("sizes_bytes", {}))

        def _healthy(sizes: dict) -> bool:
            # All baseline nodes present and non-negative: a partial post-collapse
            # snapshot (some nodes already exited) does not qualify.
            return (
                bool(base_nodes)
                and base_nodes <= set(sizes)
                and all(sizes[n] >= 0 for n in base_nodes)
            )

        # Prefer the per-level snapshots (cl_disk.json): the last level with a
        # healthy snapshot is taken before any saturation-overshoot collapse, so
        # storage growth through the knee survives even when the final level OOMs
        # the nodes. validated_total is the cumulative validated count through
        # that level. Fall back to the post-drive disk_after.json.
        disk_after = None
        validated_total = 0
        cl_disk_path = args.driver_prefix + ".cl_disk.json"
        if base_nodes and os.path.exists(cl_disk_path):
            with open(cl_disk_path, encoding="utf-8") as f:
                disk_levels = json.load(f)
            cum = 0
            for lv in disk_levels:
                cum += lv.get("completed", 0)
                sizes = lv.get("disk_bytes") or {}
                if _healthy(sizes):
                    disk_after = {"sizes_bytes": sizes}
                    validated_total = cum
        if disk_after is None and os.path.exists(disk_after_path):
            with open(disk_after_path, encoding="utf-8") as f:
                da = json.load(f)
            if _healthy(da.get("sizes_bytes") or {}):
                disk_after = da
                validated_total = sum(lv[validated_key] for lv in cl_levels)

        if disk_before is not None:
            parts.append("\n## Ledger storage growth (on disk)\n")
            disk_tbl = (
                disk_growth_table(disk_before, disk_after, validated_total)
                if disk_after is not None
                else None
            )
            if disk_tbl:
                parts.append(disk_tbl)
            else:
                # Emit a note rather than drop the section silently: a silent
                # drop once hid a hybrid run's missing storage data.
                parts.append(
                    "_Not captured for this run: the disk snapshot returned no node "
                    "sizes. Re-run to collect storage growth._"
                )

    ms_tp = sorted(
        glob.glob(os.path.join(out_dir, "run_ms*.cl_summary.json")),
        key=lambda p: int(p.split("run_ms")[1].split(".")[0]),
    )
    if ms_tp:
        rows = []
        for path in ms_tp:
            n = int(path.split("run_ms")[1].split(".")[0])
            with open(path, encoding="utf-8") as f:
                s = json.load(f)
            rows.append(
                [
                    n,
                    s.get("max_sustained_achieved_tps", "-"),
                    s.get("max_sustained_rate_tps", "-"),
                ]
            )
        parts.append("\n## Multi-sign throughput by signer count (N)\n")
        parts.append(
            "Each N runs an open-loop rate-ramp on a corpus of N-of-N hybrid "
            "multi-signed Payments. Throughput falls as N rises because the "
            "transaction grows ~3.7 KB per signer and the network is size-bound.\n"
        )
        parts.append(
            md_table(
                ["N", "max_sustained_tps", "knee_onset_rate_tps"],
                rows,
            )
        )

    ms_path = os.path.join(out_dir, "multisign.json")
    if os.path.exists(ms_path):
        with open(ms_path, encoding="utf-8") as f:
            ms = json.load(f)
        lv = ms.get("levels", [])
        if lv:
            parts.append("\n## Multi-sign verify cost by signer count (N)\n")
            parts.append(
                "Per-signer in-process verify cost (the `checkSign.multi.per_signer` "
                "probe) and the implied total per transaction, alongside the "
                "deterministic per-signer payload growth.\n"
            )
            parts.append(
                md_table(
                    [
                        "N",
                        "events",
                        "per_signer_median_us",
                        "per_signer_p95_us",
                        "total_verify_us_per_tx",
                        "payload_delta_bytes",
                    ],
                    [
                        [
                            r["N"],
                            r["probe_events"],
                            r["per_signer_median_us"],
                            r["per_signer_p95_us"],
                            r["total_verify_us_per_tx"],
                            r["payload_delta_bytes"],
                        ]
                        for r in lv
                    ],
                )
            )

    parts.append("\n## In-process probe timings\n")
    parts.append(
        "Per-tag in-process timings (us), pooled across all nodes: the `checkSign`"
        " / `validation.verify` / `manifest.verify` probes are signature"
        " verification; `tx.deserialize`, `ledger.tx_insert`, and `ledger.persist`"
        " are the size-proportional processing steps (parse, tx-tree hash, and"
        " per-ledger nodestore flush); `ledger.apply_set` is the whole serial"
        " per-round ledger build (the apply loop) that those per-tx steps sum"
        " into -- the non-parallel critical path that bounds throughput.\n"
    )
    parts.append(verify_overhead_table(perf) if perf else "_(no probe events found)_")
    if perf:
        parts.append("\n### Full distribution (us per probe)\n")
        parts.append(
            "Mean, sample standard deviation, and percentiles per probe, pooled "
            "across all nodes (the same events as the table above).\n"
        )
        parts.append(distribution_table(perf))

    parts.append("\n## Resource usage\n")
    parts.append(resource_table(monitor) if monitor else "_(no monitor samples found)_")

    if args.microbench and os.path.exists(args.microbench):
        with open(args.microbench, encoding="utf-8") as f:
            mb = json.load(f)

        timings = mb.get("timings", {})
        c = timings.get("c", {})
        avx2 = timings.get("avx2", {})

        def to_us(t: dict):
            if not t or t.get("real_ns") is None:
                return None
            factor = {"ns": 1e-3, "us": 1.0, "ms": 1e3, "s": 1e6}.get(
                t.get("unit", "ns"), 1e-3
            )
            return t["real_ns"] * factor

        def cell(backend: dict, name: str) -> str:
            v = to_us(backend.get(name, {}))
            return f"{v:.1f}" if v is not None else "-"

        if c:
            parts.append("\n## Cryptographic microbench (us per op)\n")
            parts.append(
                "secp256k1 and Ed25519 are backend-independent; ML-DSA-44 is shown for "
                "the C-only and AVX2 backends.\n"
            )
            rows = [
                [
                    "sign",
                    cell(c, "BM_secp256k1_sign"),
                    cell(c, "BM_ed25519_sign"),
                    cell(c, "BM_mldsa44_sign"),
                    cell(avx2, "BM_mldsa44_sign"),
                ],
                [
                    "verify",
                    cell(c, "BM_secp256k1_verify"),
                    cell(c, "BM_ed25519_verify"),
                    cell(c, "BM_mldsa44_verify"),
                    cell(avx2, "BM_mldsa44_verify"),
                ],
            ]
            parts.append(
                md_table(
                    [
                        "operation",
                        "secp256k1",
                        "Ed25519",
                        "ML-DSA-44 (C)",
                        "ML-DSA-44 (AVX2)",
                    ],
                    rows,
                )
            )

        st = mb.get("size_table", {})
        parts.append("\n## Signature / payload sizes (bytes)\n")
        rows = []
        for alg in ("secp256k1", "ed25519", "mldsa44"):
            a = st.get(alg, {})
            rows.append(
                [alg, a.get("pubkey_bytes", "-"), a.get("signature_bytes", "-")]
            )
        parts.append(md_table(["algorithm", "pubkey_bytes", "signature_bytes"], rows))
        parts.append(
            f"\n- Per-signer hybrid delta: **{st.get('multi_sign_per_signer_delta_bytes', '-')} bytes**"
        )
        msd = st.get("multi_sign_payload_delta_bytes", {})
        if msd:
            parts.append(
                "- Multi-sign payload delta by N: "
                + ", ".join(f"N={k}: {v}" for k, v in msd.items())
            )

    report = "\n".join(parts) + "\n"
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(report)
    print(report)
    print(f"Wrote {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print(f"\nFAILED: {e}", file=sys.stderr, flush=True)
        sys.exit(1)
