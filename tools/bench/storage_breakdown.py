#!/usr/bin/env python3
"""Break the on-disk growth per transaction into its parts.

Phase 2 reported total database growth per transaction with no breakdown. This
script attributes it, for one node:

  serialized transaction   `tx` binary blob size, sampled over the run's txs
  metadata                 `tx` binary meta size (AffectedNodes carries the
                           final state of each modified AccountRoot)
  account record           `ledger_entry` binary AccountRoot size for sample
                           senders (a hybrid account carries its PQ key)
  node store (NuDB)        growth of db/nudb per transaction; the remainder
                           after tx + metadata is state-tree nodes (leaf and
                           inner), ledger headers and NuDB overhead
  transaction DB (SQLite)  growth of transaction.db(+wal) per transaction, split
                           into stored blob bytes (summed column lengths) and
                           index/page overhead (the rest of the file)

Inputs come from the run: the disk snapshots from measure_disk.py (before and
after the drive), the driver's ledger files, and live RPC on the node. The
SQLite file is copied out of the container with `docker cp` and read locally,
because the node image has no sqlite3.

Usage:
  storage_breakdown.py --prefix OUT/steady --disk-before OUT/disk_before.json \
      --disk-after OUT/disk_after.json --url http://127.0.0.1:5005 \
      --container rippled-stock-1 --accounts OUT/accounts.json --out OUT/storage
"""

from __future__ import annotations

import argparse
import gzip
import json
import os
import random
import shutil
import sqlite3
import subprocess
import sys
import tempfile

import bench_lib as bl

DB_DIR = "/var/lib/rippled/db"


def mean(xs):
    return sum(xs) / len(xs) if xs else float("nan")


def sample_hashes(prefix, n, seed=1):
    """Reservoir-sample n transaction hashes from the run's validated ledgers."""
    rnd = random.Random(seed)
    out = []
    with gzip.open(f"{prefix}.ledger_txs.csv.gz", "rt") as f:
        next(f, None)
        for i, line in enumerate(f):
            h = line.rstrip("\n").split(",", 1)[1]
            if len(out) < n:
                out.append(h)
            else:
                j = rnd.randint(0, i)
                if j < n:
                    out[j] = h
    return out


def txs_between(prefix, t0, t1):
    total = 0
    with open(f"{prefix}.ledgers.jsonl") as f:
        for line in f:
            r = json.loads(line)
            if r.get("first_seen") and t0 <= r["first_seen"] <= t1:
                total += r.get("txn_count") or 0
    return total


def component_growth(before, after, container):
    b = (before.get("components_bytes") or {}).get(container, {})
    a = (after.get("components_bytes") or {}).get(container, {})
    keys = set(a) | set(b)
    return {k: a.get(k, 0) - b.get(k, 0) for k in sorted(keys)}


def sqlite_blob_bytes(container, workdir):
    """Copy transaction.db (+wal/shm) out and sum stored bytes per table."""
    got = []
    for name in ("transaction.db", "transaction.db-wal", "transaction.db-shm"):
        try:
            r = subprocess.run(
                ["docker", "cp", f"{container}:{DB_DIR}/{name}", os.path.join(workdir, name)],
                capture_output=True,
                timeout=600,
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        if r.returncode == 0:
            got.append(name)
    if "transaction.db" not in got:
        return None
    con = sqlite3.connect(os.path.join(workdir, "transaction.db"))
    out = {}
    try:
        tables = [r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")]
        for t in tables:
            cols = [r[1] for r in con.execute(f"PRAGMA table_info('{t}')")]
            if not cols:
                continue
            expr = " + ".join(f"IFNULL(LENGTH(\"{c}\"),0)" for c in cols)
            rows, data = con.execute(f"SELECT COUNT(*), SUM({expr}) FROM \"{t}\"").fetchone()
            out[t] = {"rows": rows, "column_bytes": data or 0}
        page_size = con.execute("PRAGMA page_size").fetchone()[0]
        page_count = con.execute("PRAGMA page_count").fetchone()[0]
        out["_file_bytes_after_wal_merge"] = page_size * page_count
        try:  # needs SQLITE_ENABLE_DBSTAT_VTAB; per-table and per-index pages
            out["_dbstat"] = {
                name: size
                for name, size in con.execute(
                    "SELECT name, SUM(pgsize) FROM dbstat GROUP BY name"
                )
            }
        except sqlite3.Error:
            pass
    finally:
        con.close()
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", required=True, help="steady_state_driver --out prefix")
    ap.add_argument("--disk-before", required=True)
    ap.add_argument("--disk-after", required=True)
    ap.add_argument("--url", default="http://127.0.0.1:5005")
    ap.add_argument("--container", default="rippled-stock-1")
    ap.add_argument("--accounts", default=None)
    ap.add_argument("--sample", type=int, default=500)
    ap.add_argument("--out", required=True, help="writes OUT.json and OUT.md")
    args = ap.parse_args()

    before = json.load(open(args.disk_before))
    after = json.load(open(args.disk_after))
    n_tx = txs_between(args.prefix, before.get("t", 0), after.get("t", 1e18))

    hashes = sample_hashes(args.prefix, args.sample)
    tx_sizes, meta_sizes = [], []
    for h in hashes:
        r = bl.rpc(args.url, "tx", {"transaction": h, "binary": True}, retries=3)
        if r.get("status") == "success" and r.get("tx_blob"):
            tx_sizes.append(len(r["tx_blob"]) // 2)
            meta_sizes.append(len(r.get("meta", "")) // 2)

    acct_sizes = []
    if args.accounts:
        accts = [a["account"] for a in json.load(open(args.accounts))["accounts"]]
        for a in random.Random(2).sample(accts, min(50, len(accts))):
            r = bl.rpc(
                args.url,
                "ledger_entry",
                {"account_root": a, "binary": True, "ledger_index": "validated"},
                retries=3,
            )
            if r.get("node_binary"):
                acct_sizes.append(len(r["node_binary"]) // 2)

    growth = component_growth(before, after, args.container)
    nudb = growth.get("nudb")
    txdb = sum(v for k, v in growth.items() if k.startswith("transaction.db"))

    workdir = tempfile.mkdtemp(prefix="txdb-")
    try:
        sq = sqlite_blob_bytes(args.container, workdir)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    per = lambda x: (x / n_tx) if (n_tx and x is not None) else None  # noqa: E731
    tx_b, meta_b = mean(tx_sizes), mean(meta_sizes)
    sq_rows = (sq or {}).get("Transactions", {}).get("rows")
    sq_data = (sq or {}).get("Transactions", {}).get("column_bytes")
    sq_acct = (sq or {}).get("AccountTransactions", {}).get("column_bytes", 0)
    sq_file = (sq or {}).get("_file_bytes_after_wal_merge")

    res = {
        "container": args.container,
        "transactions_in_interval": n_tx,
        "sampled_txs": len(tx_sizes),
        "tx_bytes_mean": round(tx_b, 1),
        "meta_bytes_mean": round(meta_b, 1),
        "account_root_bytes_mean": round(mean(acct_sizes), 1) if acct_sizes else None,
        "disk_growth_bytes": growth,
        "nodestore_growth_per_tx": per(nudb),
        "nodestore_remainder_per_tx": (per(nudb) - tx_b - meta_b) if per(nudb) is not None else None,
        "txdb_growth_per_tx": per(txdb),
        "sqlite": sq,
        "sqlite_tx_table_bytes_per_row": (sq_data / sq_rows) if sq_rows else None,
        "sqlite_account_tx_bytes_per_row": (sq_acct / sq_rows) if sq_rows else None,
        "sqlite_overhead_per_row": ((sq_file - sq_data - sq_acct) / sq_rows)
        if (sq_rows and sq_file)
        else None,
        "notes": "nodestore_remainder = state-tree leaf+inner nodes, ledger headers and "
        "NuDB overhead per tx. SQLite figures cover every row in transaction.db, "
        "including setup transactions; the growth figures cover the drive only.",
    }
    with open(args.out + ".json", "w") as f:
        json.dump(res, f, indent=2)

    def fmt(v):
        return "-" if v is None or v != v else f"{v:,.0f}"

    md = [
        f"## Storage breakdown per transaction ({args.container})\n",
        f"{n_tx:,} transactions validated between the disk snapshots; "
        f"{len(tx_sizes)} sampled for tx/metadata sizes.\n",
        "| component | bytes per tx |",
        "|---|---|",
        f"| serialized transaction | {fmt(tx_b)} |",
        f"| metadata | {fmt(meta_b)} |",
        f"| node store (NuDB) growth | {fmt(res['nodestore_growth_per_tx'])} |",
        f"| - of which state tree, headers, overhead | {fmt(res['nodestore_remainder_per_tx'])} |",
        f"| transaction DB (SQLite) growth | {fmt(res['txdb_growth_per_tx'])} |",
        f"| - Transactions table data per row | {fmt(res['sqlite_tx_table_bytes_per_row'])} |",
        f"| - AccountTransactions data per row | {fmt(res['sqlite_account_tx_bytes_per_row'])} |",
        f"| - SQLite index/page overhead per row | {fmt(res['sqlite_overhead_per_row'])} |",
        "",
        f"AccountRoot entry size (sampled senders): {fmt(res['account_root_bytes_mean'])} bytes.",
        "",
        f"_{res['notes']}_",
    ]
    with open(args.out + ".md", "w") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print(f"\nFAILED: {e}", file=sys.stderr, flush=True)
        sys.exit(1)
