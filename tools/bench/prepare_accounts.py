#!/usr/bin/env python3
"""Stand up the benchmark source-account pool.

A large pool is required because the load driver submits one in-flight
transaction per account at a time (XRPL serializes per-account by Sequence); a
small pool makes the driver itself the throughput bottleneck.
"""

from __future__ import annotations

import argparse
import json
import sys

import bench_lib as bl

FIXED_FEE = "5000"  # drops; held constant so fee escalation does not confound runs
FUND_DROPS = "100000000000"  # 100k XRP per source account


def mint_pool(url: str, size: int, hybrid: bool) -> list[dict]:
    accounts: list[dict] = []
    for i in range(size):
        ecc = bl.rpc_checked(url, "wallet_propose", {"key_type": "secp256k1"})
        rec = {
            "account": ecc["account_id"],
            "secret": ecc["master_seed"],
            "pq_seed": None,
            "pq_pubkey": None,
        }
        if hybrid:
            pq = bl.rpc_checked(url, "wallet_propose", {"key_type": "dilithium"})
            rec["pq_seed"] = pq["pq_seed_hex"]
            rec["pq_pubkey"] = pq["public_key_hex"]
        accounts.append(rec)
        if (i + 1) % 50 == 0:
            print(f"  minted {i + 1}/{size}", flush=True)
    return accounts


# All fundings come from the single genesis account, sharing one per-account
# queue; submitting hundreds at once overflows it (telCAN_NOT_QUEUE_FULL), so
# fundings are batched to fit the open ledger, validating each before the next.
FUND_BATCH = 20


def fund_pool(url: str, accounts: list[dict]) -> None:
    # Track the genesis Sequence locally rather than re-reading per batch: a
    # re-read of "current" can hit a node lagging the validated ledger and return
    # a stale, too-low sequence that collides as tefPAST_SEQ. Genesis is the sole
    # funding source, so its sequence advances by exactly one per accepted Payment.
    seq = bl.account_sequence(url, bl.GENESIS_ACCOUNT)
    for start in range(0, len(accounts), FUND_BATCH):
        batch = accounts[start : start + FUND_BATCH]
        for rec in batch:
            r = bl.rpc_checked(
                url,
                "submit",
                {
                    "secret": bl.GENESIS_SECRET,
                    "tx_json": {
                        "TransactionType": "Payment",
                        "Account": bl.GENESIS_ACCOUNT,
                        "Destination": rec["account"],
                        "Amount": FUND_DROPS,
                        "Sequence": seq,
                        "Fee": FIXED_FEE,
                    },
                },
            )
            if r.get("engine_result") not in ("tesSUCCESS", "terQUEUED"):
                raise bl.RpcError(f"fund {rec['account']}: {r.get('engine_result')}")
            seq += 1
        bl.wait_ledgers(url, 2)
        print(
            f"  funded {min(start + FUND_BATCH, len(accounts))}/{len(accounts)}",
            flush=True,
        )


def opt_in_pool(url: str, accounts: list[dict]) -> None:
    """Opt every account in to hybrid via a hybrid-signed AccountSet."""
    for start in range(0, len(accounts), FUND_BATCH):
        batch = accounts[start : start + FUND_BATCH]
        for rec in batch:
            seq = bl.account_sequence(url, rec["account"])
            signed = bl.rpc_checked(
                url,
                "sign",
                {
                    "secret": rec["secret"],
                    "pq_seed_hex": rec["pq_seed"],
                    "tx_json": {
                        "TransactionType": "AccountSet",
                        "Account": rec["account"],
                        "SetFlag": bl.ASF_QUANTUM,
                        "QuantumPubKey": rec["pq_pubkey"],
                        "Sequence": seq,
                        "Fee": FIXED_FEE,
                    },
                },
            )
            r = bl.rpc_checked(url, "submit", {"tx_blob": signed["tx_blob"]})
            if r.get("engine_result") not in ("tesSUCCESS", "terQUEUED"):
                raise bl.RpcError(f"opt-in {rec['account']}: {r.get('engine_result')}")
        bl.wait_ledgers(url, 2)
        print(
            f"  opted in {min(start + FUND_BATCH, len(accounts))}/{len(accounts)}",
            flush=True,
        )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:5005")
    ap.add_argument("--pool-size", type=int, default=200)
    ap.add_argument(
        "--hybrid", action="store_true", help="mint PQ keys and opt accounts in"
    )
    ap.add_argument("--out", default="accounts.json")
    args = ap.parse_args()

    print(f"Waiting for {args.url} ...", flush=True)
    bl.wait_for_server(args.url)

    print(f"Minting {args.pool_size} accounts (hybrid={args.hybrid}) ...", flush=True)
    accounts = mint_pool(args.url, args.pool_size, args.hybrid)

    print("Funding pool ...", flush=True)
    fund_pool(args.url, accounts)

    if args.hybrid:
        print("Opting pool in to hybrid ...", flush=True)
        opt_in_pool(args.url, accounts)

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump({"hybrid": args.hybrid, "accounts": accounts}, f)
    print(f"Wrote {len(accounts)} accounts to {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print(f"\nFAILED: {e}", file=sys.stderr, flush=True)
        sys.exit(1)
