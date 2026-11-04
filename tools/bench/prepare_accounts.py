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
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import bench_lib as bl

# Held constant across configurations. With xrpld's default transaction-queue
# settings (the mainnet recommended configuration) a high fee keeps the funding
# bursts in the open ledger instead of the per-account queue.
FIXED_FEE = "1000000"
FUND_DROPS = "100000000000"  # 100k XRP per source account


def mint_pool(url: str, size: int, hybrid: bool, threads: int = 16) -> list[dict]:
    def one(_i: int) -> dict:
        ecc = bl.rpc_checked(url, "wallet_propose", {"key_type": "secp256k1"}, retries=5)
        rec = {
            "account": ecc["account_id"],
            "secret": ecc["master_seed"],
            "pq_seed": None,
            "pq_pubkey": None,
        }
        if hybrid:
            pq = bl.rpc_checked(url, "wallet_propose", {"key_type": "dilithium"}, retries=5)
            rec["pq_seed"] = pq["pq_seed_hex"]
            rec["pq_pubkey"] = pq["public_key_hex"]
        return rec

    accounts: list[dict] = []
    with ThreadPoolExecutor(max_workers=threads) as ex:
        for n, rec in enumerate(ex.map(one, range(size))):
            accounts.append(rec)
            if (n + 1) % 1000 == 0:
                print(f"  minted {n + 1}/{size}", flush=True)
    return accounts


def validated_seq(url: str, account: str) -> int | None:
    """Validated Sequence, or None if the account is not in the validated ledger."""
    r = bl.rpc(url, "account_info", {"account": account, "ledger_index": "validated"}, retries=3)
    if r.get("status") != "success":
        return None
    return int(r["account_data"]["Sequence"])


def pay_chain(
    url: str,
    secret: str,
    source: str,
    payments: list[tuple[str, str]],
    fee: str,
    chunk: int = 200,
    timeout_s: float = 60.0,
) -> None:
    """Submit Payments from one source with explicit, consecutive Sequences,
    `chunk` at a time, waiting for each chunk to validate before the next.
    Anything that did not make it is re-submitted by Sequence (the signature
    is deterministic, so a resubmit is the same transaction)."""
    seq0 = validated_seq(url, source)
    if seq0 is None:
        raise bl.RpcError(f"source {source} not funded")
    for start in range(0, len(payments), chunk):
        part = payments[start : start + chunk]
        first = seq0 + start
        end = first + len(part)
        for attempt in range(6):
            cur = validated_seq(url, source) or first
            if cur >= end:
                break
            for k in range(max(cur, first) - first, len(part)):
                dest, amount = part[k]
                r = bl.rpc(
                    url,
                    "submit",
                    {
                        "secret": secret,
                        "tx_json": {
                            "TransactionType": "Payment",
                            "Account": source,
                            "Destination": dest,
                            "Amount": amount,
                            "Sequence": first + k,
                            "Fee": fee,
                        },
                    },
                    retries=5,
                )
                eng = r.get("engine_result", "")
                if eng.startswith(("tem", "tef")) and eng != "tefPAST_SEQ":
                    raise bl.RpcError(f"pay {source}->{dest}: {eng}")
            deadline = time.monotonic() + timeout_s
            while time.monotonic() < deadline:
                if (validated_seq(url, source) or 0) >= end:
                    break
                time.sleep(1.0)
            if (validated_seq(url, source) or 0) >= end:
                break
        else:
            raise bl.RpcError(f"{source}: payments {first}..{end - 1} did not validate")


def fund_pool(
    url: str, accounts: list[dict], fee: str, funders: int, threads: int
) -> None:
    """Fan-out funding: genesis funds `funders` intermediate accounts, each of
    which funds its slice of the pool in parallel. One source account can only
    advance one Sequence at a time, so funding 20k accounts from genesis alone
    takes hours; this takes minutes."""
    funders = max(1, min(funders, len(accounts)))
    fund_wallets = [
        bl.rpc_checked(url, "wallet_propose", {"key_type": "secp256k1"}) for _ in range(funders)
    ]
    slices = [accounts[i::funders] for i in range(funders)]
    per_funder = [
        str(int(FUND_DROPS) * len(sl) + int(fee) * (len(sl) + 10) + 100_000_000)
        for sl in slices
    ]
    print(f"  genesis -> {funders} funders", flush=True)
    pay_chain(
        url,
        bl.GENESIS_SECRET,
        bl.GENESIS_ACCOUNT,
        [(w["account_id"], amt) for w, amt in zip(fund_wallets, per_funder)],
        fee,
    )
    print(f"  funders -> {len(accounts)} accounts ({threads} in parallel)", flush=True)
    with ThreadPoolExecutor(max_workers=min(threads, funders)) as ex:
        futs = [
            ex.submit(
                pay_chain,
                url,
                w["master_seed"],
                w["account_id"],
                [(rec["account"], FUND_DROPS) for rec in sl],
                fee,
            )
            for w, sl in zip(fund_wallets, slices)
        ]
        for n, f in enumerate(as_completed(futs)):
            f.result()
            if (n + 1) % 10 == 0 or n + 1 == len(futs):
                print(f"  funder chains done {n + 1}/{len(futs)}", flush=True)


def opt_in_pool(url: str, accounts: list[dict], fee: str, threads: int) -> None:
    """Opt every account in to hybrid via a hybrid-signed AccountSet, in
    parallel (one transaction per account, so no ordering constraint), then
    re-submit any that did not validate."""

    def submit_one(rec: dict) -> None:
        seq = rec["_seq"]
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
                    "Fee": fee,
                },
            },
            retries=8,
        )
        r = bl.rpc(url, "submit", {"tx_blob": signed["tx_blob"]}, retries=5)
        eng = r.get("engine_result", "")
        if eng.startswith(("tem", "tef")) and eng != "tefPAST_SEQ":
            raise bl.RpcError(f"opt-in {rec['account']}: {eng}")

    with ThreadPoolExecutor(max_workers=threads) as ex:
        for rec, s in zip(accounts, ex.map(lambda r: validated_seq(url, r["account"]), accounts)):
            rec["_seq"] = s
    todo = list(accounts)
    for attempt in range(5):
        with ThreadPoolExecutor(max_workers=threads) as ex:
            list(ex.map(submit_one, todo))
        bl.wait_ledgers(url, 3, timeout_s=120)
        with ThreadPoolExecutor(max_workers=threads) as ex:
            now = list(ex.map(lambda r: validated_seq(url, r["account"]), todo))
        todo = [r for r, s in zip(todo, now) if (s or 0) <= r["_seq"]]
        print(f"  opt-in round {attempt + 1}: {len(accounts) - len(todo)}/{len(accounts)} done", flush=True)
        if not todo:
            break
    if todo:
        raise bl.RpcError(f"{len(todo)} accounts failed to opt in")
    for rec in accounts:
        rec.pop("_seq", None)


def verify_funded(url: str, accounts: list[dict], threads: int) -> None:
    with ThreadPoolExecutor(max_workers=threads) as ex:
        seqs = list(ex.map(lambda r: validated_seq(url, r["account"]), accounts))
    missing = [r["account"] for r, s in zip(accounts, seqs) if s is None]
    if missing:
        raise bl.RpcError(f"{len(missing)} accounts not funded, e.g. {missing[:3]}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:5005")
    ap.add_argument("--pool-size", type=int, default=200)
    ap.add_argument(
        "--hybrid", action="store_true", help="mint PQ keys and opt accounts in"
    )
    ap.add_argument("--out", default="accounts.json")
    ap.add_argument("--fee", default=FIXED_FEE, help="fee in drops for setup transactions")
    ap.add_argument("--funders", type=int, default=0, help="intermediate funders (0 = pool/200, max 100)")
    ap.add_argument("--threads", type=int, default=32)
    args = ap.parse_args()

    print(f"Waiting for {args.url} ...", flush=True)
    bl.wait_for_server(args.url)

    print(f"Minting {args.pool_size} accounts (hybrid={args.hybrid}) ...", flush=True)
    accounts = mint_pool(args.url, args.pool_size, args.hybrid, args.threads)

    print("Funding pool ...", flush=True)
    funders = args.funders or min(100, max(1, args.pool_size // 200))
    fund_pool(args.url, accounts, args.fee, funders, args.threads)
    verify_funded(args.url, accounts, args.threads)

    if args.hybrid:
        print("Opting pool in to hybrid ...", flush=True)
        opt_in_pool(args.url, accounts, args.fee, args.threads)

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
