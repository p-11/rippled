#!/usr/bin/env python3
"""End-to-end smoke test for the hybrid-PQ DevNet.

Talks to stock-1's admin RPC on http://127.0.0.1:5005 and exercises:

  1. wallet_propose for a fresh ECC keypair (alice).
  2. wallet_propose key_type=dilithium for a PQ keypair (also alice).
  3. Fund alice from the genesis master via a Payment.
  4. Opt alice in to hybrid via AccountSet asfQuantum.
  5. Verify alice's AccountRoot now carries sfQuantumPubKey.
  6. Send a hybrid Payment from alice and verify it lands.
  7. Send an ECC-only Payment from alice and verify it is rejected
     with tefBAD_AUTH (the binding check the protocol enforces once
     the account is opted in).

Exit code 0 on success, non-zero on the first failed assertion.

BEFORE RE-RUNNING: always tear down with `docker compose down -v`
(or `bash scripts/devnet/clean.sh`). The Quantum amendment is
pre-enabled via the validators' --start flag, which only takes
effect on a fresh database; restarting against persisted volumes
leaves Quantum in the normal 2-week voting cycle and this script
will time out at `wait_for_quantum_amendment`.
"""

from __future__ import annotations

import json
import sys
import time
import urllib.error
import urllib.request

RPC_URL = "http://127.0.0.1:5005"
GENESIS_SECRET = "masterpassphrase"
ASF_QUANTUM = 18  # AccountSet flag from include/xrpl/protocol/TxFlags.h


def rpc(method: str, params: dict, *, expect_success: bool = True) -> dict:
    body = json.dumps({"method": method, "params": [params]}).encode()
    req = urllib.request.Request(
        RPC_URL, data=body, headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        payload = json.loads(resp.read())
    result = payload.get("result", {})
    if expect_success and result.get("status") != "success":
        raise RuntimeError(f"{method} failed: {json.dumps(result, indent=2)}")
    return result


def wait_for_node(timeout_s: int = 120) -> None:
    print(f"Waiting for node at {RPC_URL} ...", flush=True)
    deadline = time.monotonic() + timeout_s
    last_err = None
    while time.monotonic() < deadline:
        try:
            r = rpc("server_info", {})
            state = r.get("info", {}).get("server_state", "")
            print(f"  stock-1 server_state={state}", flush=True)
            if state in {"proposing", "full", "tracking"}:
                return
        except (urllib.error.URLError, RuntimeError) as e:
            last_err = e
        time.sleep(2)
    raise RuntimeError(
        f"stock-1 did not reach steady state within {timeout_s}s ({last_err})"
    )


def wait_for_quantum_amendment(timeout_s: int = 240) -> None:
    print("Waiting for Quantum amendment to activate ...", flush=True)
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            r = rpc("feature", {"feature": "Quantum"})
            # `feature` returns amendments keyed by hash at the top level of
            # result; only "status" is a sibling, so filter it out.
            amendments = {k: v for k, v in r.items() if isinstance(v, dict)}
            quantum = next(iter(amendments.values()), {})
            enabled = quantum.get("enabled", False)
            print(f"  Quantum amendment enabled={enabled}", flush=True)
            if enabled:
                return
        except RuntimeError as e:
            print(f"  feature query: {e}", flush=True)
        time.sleep(4)
    raise RuntimeError(f"Quantum amendment did not activate within {timeout_s}s")


def fund(dest_account: str, amount_drops: str) -> None:
    print(f"Funding {dest_account} with {amount_drops} drops ...", flush=True)
    r = rpc(
        "submit",
        {
            "secret": GENESIS_SECRET,
            "tx_json": {
                "TransactionType": "Payment",
                "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                "Destination": dest_account,
                "Amount": amount_drops,
            },
        },
    )
    if r.get("engine_result") != "tesSUCCESS":
        raise RuntimeError(f"funding failed: {r.get('engine_result')}")
    _close_ledger()


def _close_ledger() -> None:
    # Standalone mode auto-closes; networked mode needs a brief wait for
    # validators to agree.
    time.sleep(5)


def sign_and_submit(
    secret: str,
    tx_json: dict,
    *,
    pq_seed_hex: str | None = None,
    expect_engine: str = "tesSUCCESS",
) -> dict:
    params = {"secret": secret, "tx_json": tx_json}
    if pq_seed_hex is not None:
        params["pq_seed_hex"] = pq_seed_hex
    s = rpc("sign", params)
    blob = s["tx_blob"]
    r = rpc("submit", {"tx_blob": blob})
    actual = r.get("engine_result")
    if actual != expect_engine:
        raise RuntimeError(
            f"submit engine_result={actual} (expected {expect_engine})\n"
            f"full response: {json.dumps(r, indent=2)}"
        )
    _close_ledger()
    return r


def main() -> int:
    wait_for_node()
    wait_for_quantum_amendment()

    print("Minting ECC keypair for alice ...", flush=True)
    alice_ecc = rpc("wallet_propose", {"key_type": "secp256k1"})
    alice = alice_ecc["account_id"]
    alice_secret = alice_ecc["master_seed"]
    print(f"  alice={alice}", flush=True)

    print("Minting PQ keypair for alice ...", flush=True)
    alice_pq = rpc("wallet_propose", {"key_type": "dilithium"})
    alice_pq_seed = alice_pq["pq_seed_hex"]
    alice_pq_pubkey = alice_pq["public_key_hex"]
    print(
        f"  alice PQ pubkey (first 32 hex chars): {alice_pq_pubkey[:32]}...", flush=True
    )

    fund(alice, "1000000000")  # 1000 XRP

    print("Opting alice in to hybrid (AccountSet asfQuantum) ...", flush=True)
    sign_and_submit(
        alice_secret,
        {
            "TransactionType": "AccountSet",
            "Account": alice,
            "SetFlag": ASF_QUANTUM,
            "QuantumPubKey": alice_pq_pubkey,
        },
        pq_seed_hex=alice_pq_seed,
    )

    print("Verifying alice's AccountRoot carries sfQuantumPubKey ...", flush=True)
    info = rpc("account_info", {"account": alice, "ledger_index": "validated"})
    acct = info.get("account_data", {})
    if "QuantumPubKey" not in acct:
        raise RuntimeError(
            "account_info: alice is missing QuantumPubKey after opt-in:\n"
            + json.dumps(acct, indent=2)
        )
    if acct["QuantumPubKey"] != alice_pq_pubkey:
        raise RuntimeError(
            f"account_info: alice's QuantumPubKey mismatches "
            f"({acct['QuantumPubKey'][:32]}... vs {alice_pq_pubkey[:32]}...)"
        )
    print("  PQ pubkey matches", flush=True)

    print("Sending a hybrid Payment from alice -> genesis ...", flush=True)
    sign_and_submit(
        alice_secret,
        {
            "TransactionType": "Payment",
            "Account": alice,
            "Destination": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
            "Amount": "100000000",  # 100 XRP
        },
        pq_seed_hex=alice_pq_seed,
    )

    print("Trying ECC-only Payment from alice (expect tefBAD_AUTH) ...", flush=True)
    sign_and_submit(
        alice_secret,
        {
            "TransactionType": "Payment",
            "Account": alice,
            "Destination": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
            "Amount": "100000000",
        },
        expect_engine="tefBAD_AUTH",
    )

    print("\nSmoke test passed.", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print(f"\nFAILED: {e}", file=sys.stderr, flush=True)
        sys.exit(1)
