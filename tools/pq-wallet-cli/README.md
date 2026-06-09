# pq-wallet-cli

## Build

The tool is gated behind an opt-in CMake option so the default rippled
build is untouched:

```sh
cmake -Dpq_wallet_cli=ON --preset=conan-debug
cmake --build .build --target xrpld pq-wallet-cli -j$(nproc)
```

The binary lands at `.build/pq-wallet-cli`. The end-to-end demo script
expects `.build/xrpld` to exist too.

## Quick start: end-to-end acceptance demo

```sh
tools/pq-wallet-cli/run-demo.sh
```

Launches rippled in standalone mode with the `Quantum` amendment
pre-enabled at genesis, then walks the PoC project description flow:

1. `keygen` - wallet mints fresh ECC + PQ key material.
2. Fund the wallet account from genesis (a curl-driven `Payment` to
   bootstrap the balance).
3. `opt-in` - wallet `AccountSet asfQuantum` registers the PQ
   pubkey on its own `AccountRoot`.
4. `sign-tx` + `submit-tx` - wallet hybrid-signs a `Payment`
   back to genesis and the server accepts it.
5. `show-account` - wallet verifies the on-ledger
   `QuantumPubKey` matches its local copy.
6. `RIPPLE_CUSTODY_FAIL=1 sign-tx` + `submit-tx` - the mock
   returns a deliberately corrupted ECC signature; the server
   rejects the submission. This step is load-bearing: it shows that
   acceptance in the happy path is gated by the server-side hybrid
   verification, not by anything the wallet does on its own behalf.

Exit code `0` on full success.

## Subcommands

```text
pq-wallet-cli keygen        [--wallet <path>] [--import-pq-seed <hex>]
pq-wallet-cli opt-in        [--wallet <path>] [--rpc-url <url>] [--sequence <n>] [--fee <n>]
pq-wallet-cli sign-tx       [--wallet <path>] [--rpc-url <url>] [--out <path>]
                            --to <address> --amount-drops <n>
                            [--sequence <n>] [--fee <n>]
pq-wallet-cli submit-tx     [--rpc-url <url>] (--blob <hex> | --in <path>)
pq-wallet-cli show-account  [--wallet <path>] [--rpc-url <url>]
```

`--wallet` defaults to `./pq-wallet.json`. `--rpc-url` defaults to
`http://127.0.0.1:5050` (the standalone-mode demo); pass
`http://127.0.0.1:5005` for the multi-validator DevNet.

`keygen` generates a fresh `xrpl::Seed` for the ECC half and derives
the PQ seed via SHA-512/256 of that seed (mirroring `wallet_propose
key_type=dilithium`), or accepts a raw 32-byte PQ seed via
`--import-pq-seed` so an operator can rebuild the wallet around a
server-minted key. Neither secret reaches `./pq-wallet.json`; both
land in the corresponding mock-owned state files.

`sign-tx` writes two artifacts: the raw `tx_blob` hex on stdout, and
a JSON sidecar (default `./signed-tx.json`) carrying the five fields
the PoC project description calls for (algorithm identifier, PQ
public key, PQ signature, ECC signature, serialized payload) plus
the `tx_blob` and `tx_hash` for convenience.

## Module boundary: the ECC custody mock

`EccCustodyMock` (`ecc_custody_mock.{h,cpp}`) stands in for the
Ripple Custody Application. The wallet code reaches it through three
operations:

- `initializeFromSeed(seed, key_type)` - create fresh custody
  material.
- `publicKey()` - return the public half so the wallet can
  compute account IDs and embed `sfSigningPubKey` on signed
  transactions.
- `signWithECC(payload)` - the blackbox sign request. In
  production this is a network call to the MPC + HSM-backed Custody
  service; in this PoC it loads its locally persisted secret and
  calls `xrpl::sign` in-process.

The mock persists its state to `./pq-wallet.custody.json` (a sibling
of `./pq-wallet.json`). The wallet binary never opens that file
directly; only the mock module does. The boundary is enforced at the
filesystem level so the swap-for-real seam stays honest even under a
sloppy reading of the wallet code.

Setting `RIPPLE_CUSTODY_FAIL=1` in the environment makes the mock
return a deliberately corrupted signature, which `run-demo.sh` uses
to demonstrate that the server's hybrid verification is what gates
acceptance.

## Module boundary: the PQ custody mock

`PqCustodyMock` (`pq_custody_mock.{h,cpp}`) is the symmetric module
on the PQ side, standing in for the "parallel key path that could
later be backed by a PQ-capable module" the PoC project description asks
for. It exposes the same shape as the ECC custody mock
(`initializeFromPqSeed`, `publicKey`, `signWithPq`) so the
production two-key custody model is visible directly in the code
structure. Both mocks live in the same `pqwallet::custody` namespace.

Persistence stores only the 32-byte PQ seed; `xrpl::pqKeypair`
deterministically expands it back to the full 1,312-byte ML-DSA-44
public key and 2,560-byte secret key on load. State lives in
`./pq-wallet.pq.json` with the same wallet-binary-never-opens-it
discipline.

Setting `PQ_CUSTODY_FAIL=1` mirrors `RIPPLE_CUSTODY_FAIL` for the
PQ side.
