# Local hybrid PQ signing and verification demo

A single command runs rippled in standalone mode with the Quantum
amendment pre-enabled at genesis and walks the user-visible hybrid
signing path end-to-end. No Docker, no multi-validator setup.

## What it shows

1. Mint an ECC keypair (`wallet_propose key_type=secp256k1`).
2. Mint an ML-DSA-44 keypair (`wallet_propose key_type=dilithium`).
3. Fund the account from the genesis master.
4. Opt the account in to hybrid via `AccountSet asfQuantum`.
5. Verify the on-ledger `AccountRoot` now carries `QuantumPubKey`.
6. Send a hybrid Payment and inspect the on-ledger transaction to
   show both signatures (ECC + ML-DSA-44) and both pubkeys side by
   side.
7. Attempt an ECC-only Payment from the same opted-in account; the
   protocol rejects it with `tefBAD_AUTH`.

## Requirements

A built `xrpld` binary. From the repo root:

```sh
cmake --build .build --target xrpld -j$(nproc)
```

## Run

```sh
python3 scripts/demo/demo.py
```

The script wipes `/tmp/rippled-pq-demo`, launches `xrpld` with
`--start -a` against `scripts/demo/demo.cfg` (admin RPC on
`127.0.0.1:5050`), runs the seven steps above, then shuts the node
down. Exit code 0 on success.

Pass an explicit path to a different `xrpld` binary as the first
argument, or set the `XRPLD` env var.

The admin port (5050) was chosen to avoid colliding with the DevNet
on 5005, so both can be brought up independently.
