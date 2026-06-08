#!/usr/bin/env python3
"""Local hybrid-PQ signing and verification demo.

Spins up a single rippled in standalone mode with the Quantum amendment
pre-enabled at genesis, then walks the user-visible hybrid path:

  1. Mint an ECC keypair for alice via wallet_propose (secp256k1).
  2. Mint an ML-DSA-44 keypair for alice via wallet_propose
     key_type=dilithium.
  3. Fund alice from the genesis master.
  4. Opt alice in to hybrid via AccountSet asfQuantum, registering
     the PQ pubkey on alice's AccountRoot.
  5. Confirm AccountRoot now carries QuantumPubKey.
  6. Send a hybrid Payment from alice (expects tesSUCCESS); inspect
     the on-ledger transaction to show both signatures and both
     pubkeys side by side.
  7. Attempt an ECC-only Payment from alice with the same secret
     (expects tefBAD_AUTH; the protocol's authentication check
     enforces the binding once the account is opted in).

Exit code 0 on success, non-zero on the first failed assertion.

Usage:
  scripts/demo/demo.py [path-to-xrpld]

If the path is omitted, defaults to .build/xrpld relative to the repo
root, or the XRPLD env var if set.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ADMIN_URL = "http://127.0.0.1:5050"
DATA_DIR = Path("/tmp/rippled-pq-demo")
CFG = Path(__file__).resolve().parent / "demo.cfg"

GENESIS = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"
GENESIS_SECRET = "masterpassphrase"
ASF_QUANTUM = 18


def rpc(method: str, params: dict | None = None) -> dict:
    body = json.dumps({"method": method, "params": [params or {}]}).encode()
    req = urllib.request.Request(
        ADMIN_URL, data=body, headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        payload = json.loads(resp.read())
    return payload["result"]


def wait_for_rpc(timeout_s: int = 30) -> None:
    deadline = time.monotonic() + timeout_s
    last_err: Exception | None = None
    while time.monotonic() < deadline:
        try:
            r = rpc("server_info")
            if r.get("status") == "success":
                return
        except (urllib.error.URLError, ConnectionError, KeyError) as e:
            last_err = e
        time.sleep(0.5)
    raise RuntimeError(f"rippled did not start within {timeout_s}s ({last_err})")


def accept_ledger() -> None:
    rpc("ledger_accept")
    time.sleep(0.1)


def sign(secret: str, tx_json: dict, pq_seed_hex: str | None = None) -> dict:
    params = {"secret": secret, "tx_json": tx_json}
    if pq_seed_hex is not None:
        params["pq_seed_hex"] = pq_seed_hex
    r = rpc("sign", params)
    if r.get("status") != "success":
        raise RuntimeError(f"sign failed: {json.dumps(r, indent=2)}")
    return r


def submit(tx_blob: str, *, expect: str = "tesSUCCESS") -> dict:
    r = rpc("submit", {"tx_blob": tx_blob})
    actual = r.get("engine_result")
    if actual != expect:
        raise RuntimeError(
            f"submit engine_result={actual} (expected {expect})\n"
            f"full response: {json.dumps(r, indent=2)}"
        )
    accept_ledger()
    return r


def banner(step: str, title: str) -> None:
    print(f"\n{step}: {title}", flush=True)


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    xrpld = (
        sys.argv[1]
        if len(sys.argv) > 1
        else os.environ.get("XRPLD", str(repo_root / ".build" / "xrpld"))
    )
    if not Path(xrpld).is_file():
        print(
            f"xrpld not found at {xrpld}.\n"
            "Build it with: cmake --build .build --target xrpld -j$(nproc)",
            file=sys.stderr,
        )
        return 1

    if DATA_DIR.exists():
        shutil.rmtree(DATA_DIR)
    (DATA_DIR / "db").mkdir(parents=True)

    print(
        f"Launching {xrpld} in standalone mode\n"
        f"  config: {CFG}\n"
        f"  data:   {DATA_DIR}\n"
        f"  admin:  {ADMIN_URL}\n"
        "  flags:  --start (fresh genesis), -a (standalone)\n",
        flush=True,
    )
    proc = subprocess.Popen(
        [xrpld, "--conf", str(CFG), "--start", "-a"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        wait_for_rpc()
        # Close the genesis ledger so account_info etc. return validated data.
        accept_ledger()

        # Confirm Quantum is active at genesis.
        feat = rpc("feature", {"feature": "Quantum"})
        amendments = {k: v for k, v in feat.items() if isinstance(v, dict)}
        quantum = next(iter(amendments.values()), {})
        if not quantum.get("enabled"):
            raise RuntimeError(f"Quantum not enabled at genesis: {feat}")
        print("Quantum amendment is enabled at genesis.\n", flush=True)

        banner("Step 1/7", "Mint ECC keypair for alice")
        ecc = rpc("wallet_propose", {"key_type": "secp256k1"})
        alice = ecc["account_id"]
        alice_secret = ecc["master_seed"]
        print(f"  account_id      = {alice}")
        print(f"  key_type        = {ecc.get('key_type')}")

        banner("Step 2/7", "Mint PQ keypair for alice (ML-DSA-44)")
        pq = rpc("wallet_propose", {"key_type": "dilithium"})
        pq_seed = pq["pq_seed_hex"]
        pq_pub = pq["public_key_hex"]
        pq_sec = pq["secret_key_hex"]
        print(f"  pq_seed_hex     = {pq_seed[:32]}... ({len(pq_seed)//2} bytes)")
        print(f"  public_key_hex  = {pq_pub[:32]}... ({len(pq_pub)//2} bytes)")
        print(f"  secret_key_hex  = {pq_sec[:32]}... ({len(pq_sec)//2} bytes)")

        banner("Step 3/7", "Fund alice with 1000 XRP from genesis")
        signed = sign(
            GENESIS_SECRET,
            {
                "TransactionType": "Payment",
                "Account": GENESIS,
                "Destination": alice,
                "Amount": "1000000000",
            },
        )
        r = submit(signed["tx_blob"])
        print(f"  engine_result   = {r['engine_result']}")

        banner("Step 4/7", "Opt alice in to hybrid (AccountSet asfQuantum)")
        signed = sign(
            alice_secret,
            {
                "TransactionType": "AccountSet",
                "Account": alice,
                "SetFlag": ASF_QUANTUM,
                "QuantumPubKey": pq_pub,
            },
            pq_seed_hex=pq_seed,
        )
        r = submit(signed["tx_blob"])
        print(f"  engine_result   = {r['engine_result']}")

        banner("Step 5/7", "Verify alice's AccountRoot carries QuantumPubKey")
        info = rpc("account_info", {"account": alice, "ledger_index": "validated"})
        acct = info.get("account_data", {})
        if acct.get("QuantumPubKey") != pq_pub:
            raise RuntimeError(
                "alice's AccountRoot is missing QuantumPubKey or it does not match:\n"
                + json.dumps(acct, indent=2)
            )
        print(f"  AccountRoot.QuantumPubKey matches the minted public key")

        banner("Step 6/7", "Send hybrid Payment from alice -> genesis (100 XRP)")
        signed = sign(
            alice_secret,
            {
                "TransactionType": "Payment",
                "Account": alice,
                "Destination": GENESIS,
                "Amount": "100000000",
            },
            pq_seed_hex=pq_seed,
        )
        tx_hash = signed["tx_json"]["hash"]
        r = submit(signed["tx_blob"])
        print(f"  engine_result   = {r['engine_result']}")
        tx = rpc("tx", {"transaction": tx_hash})
        ecc_pub = tx.get("SigningPubKey", "")
        ecc_sig = tx.get("TxnSignature", "")
        pq_pub_on_tx = tx.get("QuantumPubKey", "")
        pq_sig_on_tx = tx.get("QuantumSignature", "")
        print(f"  TxnSignature     = {ecc_sig[:32]}... ({len(ecc_sig)//2} bytes, ECC)")
        print(f"  SigningPubKey    = {ecc_pub[:32]}... ({len(ecc_pub)//2} bytes, ECC)")
        print(
            f"  QuantumSignature = {pq_sig_on_tx[:32]}... ({len(pq_sig_on_tx)//2} bytes, ML-DSA-44)"
        )
        print(
            f"  QuantumPubKey    = {pq_pub_on_tx[:32]}... ({len(pq_pub_on_tx)//2} bytes, ML-DSA-44)"
        )

        banner(
            "Step 7/7",
            "Attempt ECC-only Payment from opted-in alice (expects tefBAD_AUTH)",
        )
        signed = sign(
            alice_secret,
            {
                "TransactionType": "Payment",
                "Account": alice,
                "Destination": GENESIS,
                "Amount": "100000000",
            },
        )
        r = submit(signed["tx_blob"], expect="tefBAD_AUTH")
        print(
            f"  engine_result   = {r['engine_result']} (protocol enforced the binding)"
        )

        print("\nDemo complete.", flush=True)
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print(f"\nFAILED: {e}", file=sys.stderr, flush=True)
        sys.exit(1)
