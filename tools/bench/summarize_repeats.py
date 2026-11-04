#!/usr/bin/env python3
"""Compare steady-state capacity across repeats and configurations.

Reads results/<config>-r<N>/steady.summary.json, groups by <config>, and
reports per configuration: capacity in each repeat, whether the repeats agree
within one load step (the review's acceptance condition), and the throughput
read from the ledgers at capacity. If two configurations are named with
--baseline/--compare, also reports the throughput cost between them.

Usage:
  summarize_repeats.py --results tools/bench/results \
      [--baseline upstream --compare hybrid] [--out sweep_summary.md]
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
from collections import defaultdict


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="tools/bench/results")
    ap.add_argument("--baseline", default="upstream")
    ap.add_argument("--compare", default="hybrid")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    groups: dict[str, list] = defaultdict(list)
    for path in sorted(glob.glob(os.path.join(args.results, "*", "steady.summary.json"))):
        label = os.path.basename(os.path.dirname(path))
        m = re.match(r"(.+)-r(\d+)$", label)
        cfg, rep = (m.group(1), int(m.group(2))) if m else (label, 1)
        with open(path) as f:
            sm = json.load(f)
        groups[cfg].append((rep, sm))

    lines = ["# Steady-state capacity across repeats\n"]
    lines.append(
        "| config | repeats | capacity tx/s per repeat | step | agree within one step | "
        "ledger tps at capacity (per repeat) | tx/ledger at capacity |"
    )
    lines.append("|---|---|---|---|---|---|---|")
    medians = {}
    for cfg in sorted(groups):
        reps = sorted(groups[cfg], key=lambda x: x[0])
        caps = [(sm.get("capacity") or {}).get("rate") for _, sm in reps]
        tps = [(sm.get("capacity") or {}).get("ledger_tps") for _, sm in reps]
        txl = [(sm.get("capacity") or {}).get("txn_per_ledger_mean") for _, sm in reps]
        step = next((sm.get("rate_step") for _, sm in reps if sm.get("rate_step")), None)
        known = [c for c in caps if c is not None]
        agree = (
            "n/a"
            if not known or step is None or len(known) < len(caps)
            else ("yes" if max(known) - min(known) <= step else "NO")
        )
        good = sorted(t for t in tps if t is not None)
        if good:
            medians[cfg] = good[len(good) // 2]
        lines.append(
            f"| {cfg} | {len(reps)} | {', '.join(str(c) for c in caps)} | {step} | {agree} | "
            f"{', '.join(str(t) for t in tps)} | {', '.join(str(t) for t in txl)} |"
        )
    if args.baseline in medians and args.compare in medians and medians[args.baseline]:
        b, c = medians[args.baseline], medians[args.compare]
        lines.append(
            f"\nMedian ledger throughput at capacity: {args.baseline} {b} tps, "
            f"{args.compare} {c} tps; difference {100.0 * (b - c) / b:.1f}% "
            f"(only meaningful if both configurations agree within one step)."
        )
    text = "\n".join(lines) + "\n"
    print(text)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
