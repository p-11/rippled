#!/usr/bin/env python3
"""Pre-sign a corpus of hybrid multi-signed Payments for the throughput driver.

Signers are pool[:N]; payers are a disjoint slice, so a payer's Sequence stream
is advanced only by its own SignerListSet and Payments, never by signing for
someone else.
"""

from __future__ import annotations

import argparse
import json
import sys
from concurrent.futures import ThreadPoolExecutor

import bench_lib as bl

FIXED_FEE = "200000"  # covers the (1 + N) * base multi-sign fee at these N


def set_signer_lists(url: str, payers: list[dict], signers: list[dict]) -> None:
    quorum = len(signers)
    entries = [
        {"SignerEntry": {"Account": s["account"], "SignerWeight": 1}} for s in signers
    ]
    for start in range(0, len(payers), 20):
        batch = payers[start : start + 20]
        for payer in batch:
            seq = bl.account_sequence(url, payer["account"])
            tx = {
                "TransactionType": "SignerListSet",
                "Account": payer["account"],
                "SignerQuorum": quorum,
                "SignerEntries": entries,
                "Sequence": seq,
                "Fee": FIXED_FEE,
            }
            params = {"secret": payer["secret"], "tx_json": tx}
            if payer.get("pq_seed"):
                params["pq_seed_hex"] = payer["pq_seed"]
            signed = bl.rpc_checked(url, "sign", params, retries=8)
            r = bl.rpc_checked(url, "submit", {"tx_blob": signed["tx_blob"]})
            if r.get("engine_result") not in ("tesSUCCESS", "terQUEUED"):
                raise bl.RpcError(
                    f"SignerListSet {payer['account']}: {r.get('engine_result')}"
                )
        bl.wait_ledgers(url, 2)
        print(
            f"  signer lists set {min(start + 20, len(payers))}/{len(payers)}",
            flush=True,
        )


def sign_multisigned(url: str, payer: dict, signers: list[dict], seq: int) -> str:
    tx = {
        "TransactionType": "Payment",
        "Account": payer["account"],
        "Destination": bl.GENESIS_ACCOUNT,
        "Amount": "1000",
        "Sequence": seq,
        "Fee": FIXED_FEE,
        "SigningPubKey": "",
    }
    blob = ""
    for s in signers:
        params = {"account": s["account"], "secret": s["secret"], "tx_json": tx}
        if s.get("pq_seed"):
            params["pq_seed_hex"] = s["pq_seed"]
        res = bl.rpc_checked(url, "sign_for", params, retries=8)
        tx = res["tx_json"]
        blob = res["tx_blob"]
    return blob


def build_corpus(
    urls: list[str],
    payers: list[dict],
    signers: list[dict],
    per_account: int,
    threads: int,
) -> list[dict]:
    base_seq = {
        p["account"]: bl.account_sequence(urls[0], p["account"]) for p in payers
    }
    tasks = [
        (p, base_seq[p["account"]] + k) for k in range(per_account) for p in payers
    ]
    corpus: list[dict] = [None] * len(tasks)  # type: ignore[list-item]

    def sign_one(i: int) -> None:
        payer, seq = tasks[i]
        blob = sign_multisigned(urls[i % len(urls)], payer, signers, seq)
        corpus[i] = {"account": payer["account"], "seq": seq, "blob": blob}

    with ThreadPoolExecutor(max_workers=threads) as ex:
        for n, _ in enumerate(ex.map(sign_one, range(len(tasks)))):
            if (n + 1) % 1000 == 0:
                print(f"  signed {n + 1}/{len(tasks)} multi-sign txs", flush=True)
    return corpus


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:5005")
    ap.add_argument("--urls", default=None, help="comma-separated sign endpoints")
    ap.add_argument("--accounts", default="accounts.json")
    ap.add_argument("--n", type=int, required=True, help="signers per transaction")
    ap.add_argument("--payers", type=int, default=120)
    ap.add_argument("--per-account", type=int, default=40)
    ap.add_argument("--threads", type=int, default=24)
    ap.add_argument("--out", default="corpus.jsonl")
    args = ap.parse_args()

    with open(args.accounts, encoding="utf-8") as f:
        data = json.load(f)
    if not data.get("hybrid"):
        print("WARNING: pool is not hybrid; signers carry no PQ key.", flush=True)
    accounts = data["accounts"]
    signers = accounts[: args.n]
    payers = accounts[args.n : args.n + args.payers]
    if len(payers) < args.payers:
        raise SystemExit(
            f"pool too small: need {args.n + args.payers}, have {len(accounts)}"
        )
    urls = [u for u in (args.urls.split(",") if args.urls else [args.url]) if u]

    print(
        f"Setting {args.n}-of-{args.n} signer lists on {len(payers)} payers ...",
        flush=True,
    )
    set_signer_lists(urls[0], payers, signers)

    print(
        f"Signing {len(payers) * args.per_account} multi-sign txs "
        f"(N={args.n}) with {args.threads} threads ...",
        flush=True,
    )
    corpus = build_corpus(urls, payers, signers, args.per_account, args.threads)

    with open(args.out, "w", encoding="utf-8") as f:
        for entry in corpus:
            f.write(json.dumps(entry) + "\n")
    print(f"Wrote {len(corpus)} multi-sign blobs to {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print(f"\nFAILED: {e}", file=sys.stderr, flush=True)
        sys.exit(1)
