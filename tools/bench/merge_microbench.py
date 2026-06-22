#!/usr/bin/env python3
"""Merge the C-only and AVX2 microbench outputs into one JSON.

Each input is the raw stdout of xrpl.bench.pqc: a size-table JSON object followed
by the Google Benchmark JSON object. Sizes are taken from the C run (they are
backend-independent); timings are kept per backend for side-by-side comparison.

Usage: merge_microbench.py <raw-c.json> <raw-avx2.json|-> <out.json>
"""

from __future__ import annotations

import json
import sys


def split(path: str):
    raw = open(path, encoding="utf-8").read()
    start = raw.find("{")
    depth = 0
    end = -1
    for i, c in enumerate(raw[start:], start):
        depth += (c == "{") - (c == "}")
        if depth == 0:
            end = i + 1
            break
    size_table = json.loads(raw[start:end]).get("size_table", {})
    rest = raw[end:].strip()
    bench = json.loads(rest) if rest.startswith("{") else {"benchmarks": []}
    return size_table, bench


def timings(bench: dict) -> dict:
    """Per-benchmark timing keyed by base name.

    With --benchmark_repetitions Google Benchmark emits aggregate rows
    (median / mean / stddev); take the median and fold in the stddev. Without
    repetitions, fall back to the raw row.
    """
    entries = bench.get("benchmarks", [])
    if any(e.get("run_type") == "aggregate" for e in entries):
        out: dict = {}
        stddev: dict = {}
        for e in entries:
            if e.get("run_type") != "aggregate":
                continue
            base = e.get("run_name", e.get("name"))
            if e.get("aggregate_name") == "median":
                out[base] = {
                    "real_ns": e.get("real_time"),
                    "unit": e.get("time_unit"),
                    "items_per_second": e.get("items_per_second"),
                }
            elif e.get("aggregate_name") == "stddev":
                stddev[base] = e.get("real_time")
        for base, v in out.items():
            if base in stddev:
                v["stddev_ns"] = stddev[base]
        return out
    return {
        b["name"]: {
            "real_ns": b.get("real_time"),
            "unit": b.get("time_unit"),
            "items_per_second": b.get("items_per_second"),
        }
        for b in entries
    }


def main() -> int:
    c_sizes, c_bench = split(sys.argv[1])
    avx2 = {}
    if len(sys.argv) > 2 and sys.argv[2] not in ("-", ""):
        try:
            _, a_bench = split(sys.argv[2])
            avx2 = timings(a_bench)
        except (OSError, ValueError, json.JSONDecodeError):
            avx2 = {}
    out = {
        "size_table": c_sizes,
        "timings": {"c": timings(c_bench), "avx2": avx2},
    }
    json.dump(out, open(sys.argv[3], "w", encoding="utf-8"), indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
