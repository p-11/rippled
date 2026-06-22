#!/usr/bin/env python3
"""Drive hybrid multi-signed transactions and read the per-signer verify probe.

Measures in-process verify cost per signer under real consensus by exercising
the `checkSign.multi.per_signer` probe at several N. Probe events are read by
byte offset into each node's perf log, so no clock alignment is needed.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import statistics
import sys
import time

import bench_lib as bl

PROBE_TAG = "checkSign.multi.per_signer"
PER_SIGNER_BYTES = 3732  # sfQuantumPubKey + sfQuantumSignature, serialized
FIXED_FEE = "100000"  # above any multi-sign required fee at these N


def perf_offsets(perf_dir: str) -> dict[str, int]:
    return {
        p: os.path.getsize(p)
        for p in glob.glob(os.path.join(perf_dir, "*", "perf.log"))
    }


def read_new_durations(perf_dir: str, offsets: dict[str, int], tag: str) -> list[float]:
    durs: list[float] = []
    for path in glob.glob(os.path.join(perf_dir, "*", "perf.log")):
        try:
            with open(path, encoding="utf-8") as f:
                f.seek(offsets.get(path, 0))
                for line in f:
                    if tag not in line:
                        continue
                    try:
                        rec = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if rec.get("event") == tag and rec.get("duration_us") is not None:
                        durs.append(float(rec["duration_us"]))
        except OSError:
            continue
    return durs


def set_signer_list(url: str, payer: dict, signers: list[dict], quorum: int) -> None:
    seq = bl.account_sequence(url, payer["account"])
    tx = {
        "TransactionType": "SignerListSet",
        "Account": payer["account"],
        "SignerQuorum": quorum,
        "SignerEntries": [
            {"SignerEntry": {"Account": s["account"], "SignerWeight": 1}}
            for s in signers
        ],
        "Sequence": seq,
        "Fee": FIXED_FEE,
    }
    params = {"secret": payer["secret"], "tx_json": tx}
    if payer.get("pq_seed"):
        params["pq_seed_hex"] = payer["pq_seed"]
    signed = bl.rpc_checked(url, "sign", params)
    r = bl.rpc_checked(url, "submit", {"tx_blob": signed["tx_blob"]})
    if r.get("engine_result") not in ("tesSUCCESS", "terQUEUED"):
        raise bl.RpcError(
            f"SignerListSet {payer['account']} N={quorum}: {r.get('engine_result')}"
        )


def multisign_payment(url: str, payer: dict, signers: list[dict], seq: int) -> str:
    tx = {
        "TransactionType": "Payment",
        "Account": payer["account"],
        "Destination": bl.GENESIS_ACCOUNT,
        "Amount": "1000",
        "Sequence": seq,
        "Fee": FIXED_FEE,
        "SigningPubKey": "",
    }
    for s in signers:
        params = {"account": s["account"], "secret": s["secret"], "tx_json": tx}
        if s.get("pq_seed"):
            params["pq_seed_hex"] = s["pq_seed"]
        res = bl.rpc_checked(url, "sign_for", params)
        tx = res["tx_json"]
    r = bl.rpc_checked(url, "submit_multisigned", {"tx_json": tx})
    return r.get("engine_result", "?")


def run_level(
    url: str,
    perf_dir: str,
    payers: list[dict],
    signers: list[dict],
    n: int,
    window: int,
    target_events: int,
) -> dict:
    quorum = n
    for payer in payers:
        set_signer_list(url, payer, signers, quorum)
    bl.wait_ledgers(url, 2)

    offsets = perf_offsets(perf_dir)
    submitted = 0
    rounds = max(1, -(-target_events // (len(payers) * window * n)))  # ceil
    for _ in range(rounds):
        for payer in payers:
            seq = bl.account_sequence(url, payer["account"])
            for w in range(window):
                res = multisign_payment(url, payer, signers, seq + w)
                if res in ("tesSUCCESS", "terQUEUED"):
                    submitted += 1
        bl.wait_ledgers(url, 2)
    time.sleep(1.5)  # let the nodes flush the last probe events

    durs = read_new_durations(perf_dir, offsets, PROBE_TAG)
    med = statistics.median(durs) if durs else float("nan")
    p95 = (
        sorted(durs)[min(len(durs) - 1, int(round(0.95 * (len(durs) - 1))))]
        if durs
        else float("nan")
    )
    return {
        "N": n,
        "txs_submitted": submitted,
        "probe_events": len(durs),
        "per_signer_median_us": round(med, 1),
        "per_signer_p95_us": round(p95, 1),
        "total_verify_us_per_tx": round(med * n, 1),
        "payload_delta_bytes": n * PER_SIGNER_BYTES,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:5005")
    ap.add_argument("--accounts", default="accounts.json")
    ap.add_argument("--perf-dir", default="../../scripts/devnet/perf")
    ap.add_argument("--n-values", default="1,4,8,16,32")
    ap.add_argument("--payers", type=int, default=8)
    ap.add_argument("--window", type=int, default=4)
    ap.add_argument("--target-events", type=int, default=800)
    ap.add_argument("--out", default="multisign.json")
    args = ap.parse_args()

    with open(args.accounts, encoding="utf-8") as f:
        data = json.load(f)
    if not data.get("hybrid"):
        print("WARNING: account pool is not hybrid; signers carry no PQ key.")
    accounts = data["accounts"]
    ns = [int(x) for x in args.n_values.split(",") if x]
    max_n = max(ns)

    # Payers are disjoint from signers so a payer's Sequence is only advanced by
    # submitting, never by signing for someone else.
    signer_pool = accounts[:max_n]
    payers = accounts[max_n : max_n + args.payers]
    if len(payers) < args.payers:
        raise SystemExit(
            f"pool too small: need {max_n + args.payers} accounts, have {len(accounts)}"
        )

    bl.wait_for_server(args.url)
    levels = []
    for n in ns:
        print(f"=== multi-sign N={n} ===", flush=True)
        lvl = run_level(
            args.url,
            args.perf_dir,
            payers,
            signer_pool[:n],
            n,
            args.window,
            args.target_events,
        )
        print(
            f"  N={n}: {lvl['probe_events']} events, "
            f"per-signer median {lvl['per_signer_median_us']} us, "
            f"total/tx {lvl['total_verify_us_per_tx']} us",
            flush=True,
        )
        levels.append(lvl)

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump({"probe": PROBE_TAG, "levels": levels}, f, indent=2)
    print(f"Wrote {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print(f"\nFAILED: {e}", file=sys.stderr, flush=True)
        sys.exit(1)
