#!/usr/bin/env python3
"""Pre-sign a corpus of Payment transactions for the load driver.

Signing cost is measured separately, so the timed throughput/latency run must
not pay it: blobs are signed ahead of the run, leaving submit -> validate as the
only measured path.
"""

from __future__ import annotations

import argparse
import json
import random
import sys
from concurrent.futures import ThreadPoolExecutor

import bench_lib as bl

FIXED_FEE = "1000000"  # held constant across configurations; see prepare_accounts.py
PAY_DROPS = "1000"


def build_corpus(
    urls: list[str],
    accounts: list[dict],
    hybrid: bool,
    per_account: int,
    out_path: str,
    fee: str = FIXED_FEE,
    threads: int = 16,
    batch: int = 20000,
) -> int:
    """Sign the corpus and stream it to out_path in round-robin order.

    Signing runs in batches that are written as each completes, so memory stays
    bounded (a hybrid corpus of millions of blobs is tens of GB) and a failure
    late in a long signing pass leaves the earlier batches on disk."""
    # Seed each starting sequence from the VALIDATED ledger after convergence.
    # Reading "current" from a single node right after a funding burst returns
    # stale, too-low sequences (the node lags), so blobs sign below the real
    # on-ledger value and every submit fails tefPAST_SEQ. Spread reads across
    # endpoints so no single node is hammered.
    bl.wait_nodes_converged(urls)
    with ThreadPoolExecutor(max_workers=threads) as ex:
        seqs = list(
            ex.map(
                lambda ir: bl.account_sequence(
                    urls[ir[0] % len(urls)], ir[1]["account"], validated=True
                ),
                enumerate(accounts),
            )
        )
    base_seq = {rec["account"]: s for rec, s in zip(accounts, seqs)}

    # Order round-robin (round k of every account, then k+1) so consecutive
    # entries are distinct accounts; ex.map preserves this order in the output.
    tasks = [
        (rec, base_seq[rec["account"]] + k)
        for k in range(per_account)
        for rec in accounts
    ]

    # Payments go to a random *other* pool account (all funded), mirroring the
    # "payments between N accounts" workload rather than all-to-genesis.
    addrs = [rec["account"] for rec in accounts]

    def random_dest(sender: str) -> str:
        if len(addrs) < 2:
            return bl.GENESIS_ACCOUNT
        d = random.choice(addrs)
        while d == sender:
            d = random.choice(addrs)
        return d

    def sign_one(i: int) -> None:
        rec, seq = tasks[i]
        params = {
            "secret": rec["secret"],
            "tx_json": {
                "TransactionType": "Payment",
                "Account": rec["account"],
                "Destination": random_dest(rec["account"]),
                "Amount": PAY_DROPS,
                "Sequence": seq,
                "Fee": fee,
            },
        }
        if hybrid and rec["pq_seed"]:
            params["pq_seed_hex"] = rec["pq_seed"]
        signed = bl.rpc_checked(urls[i % len(urls)], "sign", params, retries=8)
        return {"account": rec["account"], "seq": seq, "blob": signed["tx_blob"]}

    written = 0
    with open(out_path, "w", encoding="utf-8") as f, ThreadPoolExecutor(
        max_workers=threads
    ) as ex:
        for start in range(0, len(tasks), batch):
            idx = range(start, min(start + batch, len(tasks)))
            for entry in ex.map(sign_one, idx):  # preserves round-robin order
                f.write(json.dumps(entry) + "\n")
                written += 1
            f.flush()
            print(f"  signed {written}/{len(tasks)} txs", flush=True)
    return written


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:5005")
    ap.add_argument(
        "--urls", default=None, help="comma-separated sign endpoints (default: --url)"
    )
    ap.add_argument("--accounts", default="accounts.json")
    ap.add_argument("--per-account", type=int, default=50)
    ap.add_argument("--fee", default=FIXED_FEE, help="per-tx fee in drops")
    ap.add_argument("--threads", type=int, default=16, help="concurrent signers")
    ap.add_argument("--out", default="corpus.jsonl")
    args = ap.parse_args()

    with open(args.accounts, encoding="utf-8") as f:
        data = json.load(f)
    accounts = data["accounts"]
    hybrid = data.get("hybrid", False)
    urls = [u for u in (args.urls.split(",") if args.urls else [args.url]) if u]

    print(
        f"Signing {len(accounts) * args.per_account} txs "
        f"({len(accounts)} accounts x {args.per_account}, hybrid={hybrid}) "
        f"with {args.threads} threads over {len(urls)} endpoint(s) ...",
        flush=True,
    )
    n = build_corpus(
        urls, accounts, hybrid, args.per_account, args.out, args.fee, args.threads
    )
    print(f"Wrote {n} signed blobs to {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print(f"\nFAILED: {e}", file=sys.stderr, flush=True)
        sys.exit(1)
