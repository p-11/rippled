#!/usr/bin/env python3
"""Summarize rate-ramp (Phase 2 method) runs the way the Phase 2 report did.

For each configuration (result folders named <config>-r<N>): the median and
range across repeats of the sustained throughput at the saturation knee
(`max_sustained_achieved_tps`), the change versus a baseline configuration, and
the median on-disk growth per validated transaction from each run's report.

With --reference pointing at the original Phase 2 results directory (the
`results/` folder from raw-bench-results.tgz), the same figures are computed for
it and printed side by side, so a reproduction can be checked against the
report. Absolute tps depend on the machine; compare the "vs baseline" columns.

Usage:
  summarize_ramp.py --results tools/bench/results --prefix p2- \\
      [--reference /path/to/phase2/results] [--baseline amendment-off] \\
      [--out phase2-reproduction.md]
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import statistics
import sys
from collections import defaultdict

CONFIGS = ["upstream", "amendment-off", "hybrid", "amendment-off-avx2", "hybrid-avx2"]
STORAGE_RE = re.compile(r"growth per validated transaction: \*\*(\d+) bytes\*\*")


def collect(results: str, prefix: str) -> dict[str, dict[str, list]]:
    out: dict[str, dict[str, list]] = defaultdict(lambda: {"tps": [], "storage": []})
    for path in sorted(glob.glob(os.path.join(results, "*", "run.cl_summary.json"))):
        label = os.path.basename(os.path.dirname(path))
        if not label.startswith(prefix):
            continue
        cfg = re.sub(r"-r\d+$", "", label[len(prefix) :])
        with open(path) as f:
            s = json.load(f)
        if s.get("max_sustained_achieved_tps"):
            out[cfg]["tps"].append(float(s["max_sustained_achieved_tps"]))
        rep = os.path.join(os.path.dirname(path), "report.md")
        if os.path.exists(rep):
            m = STORAGE_RE.search(open(rep).read())
            if m:
                out[cfg]["storage"].append(int(m.group(1)))
    return out


def med(xs):
    return statistics.median(xs) if xs else None


def fmt_tps(xs):
    if not xs:
        return "-"
    if len(xs) == 1:
        return f"{xs[0]:,.0f}"
    return f"{med(xs):,.0f} ({min(xs):,.0f} to {max(xs):,.0f}, n={len(xs)})"


def rel(v, base):
    if v is None or not base:
        return "-"
    return f"{100.0 * (v - base) / base:+.1f}%"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="tools/bench/results")
    ap.add_argument(
        "--prefix", default="p2-", help="label prefix of the reproduction runs"
    )
    ap.add_argument("--reference", default=None, help="original Phase 2 results dir")
    ap.add_argument("--baseline", default="amendment-off")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    new = collect(args.results, args.prefix)
    ref = collect(args.reference, "") if args.reference else {}

    lines = ["# Phase 2 method: reproduction vs. original\n"]
    lines.append(
        "Sustained tps at the rate-ramp knee (median, range across repeats) and its "
        f"change versus `{args.baseline}`; on-disk bytes per validated transaction "
        "(median). Absolute tps are machine-specific: compare the change columns.\n"
    )
    hdr = "| config | reproduction tps | vs baseline |"
    sep = "|---|---|---|"
    if ref:
        hdr += " Phase 2 tps | vs baseline |"
        sep += "---|---|"
    hdr += " reproduction bytes/tx |"
    sep += "---|"
    if ref:
        hdr += " Phase 2 bytes/tx |"
        sep += "---|"
    lines += [hdr, sep]
    nb = med(new.get(args.baseline, {}).get("tps", []))
    rb = med(ref.get(args.baseline, {}).get("tps", [])) if ref else None
    for cfg in CONFIGS:
        if cfg not in new and cfg not in ref:
            continue
        n = new.get(cfg, {"tps": [], "storage": []})
        row = f"| {cfg} | {fmt_tps(n['tps'])} | {rel(med(n['tps']), nb)} |"
        if ref:
            r = ref.get(cfg, {"tps": [], "storage": []})
            row += f" {fmt_tps(r['tps'])} | {rel(med(r['tps']), rb)} |"
        s = med(n["storage"])
        row += f" {s:,.0f} |" if s else " - |"
        if ref:
            rs = med(ref.get(cfg, {"storage": []})["storage"])
            row += f" {rs:,.0f} |" if rs else " - |"
        lines.append(row)
    text = "\n".join(lines) + "\n"
    print(text)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
