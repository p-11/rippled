# pq-wallet-cli

A standalone command-line tool that demonstrates the off-chain hybrid
ECC + ML-DSA-44 custody-signing flow against a hybrid-aware rippled.

Per the Project-11 PoC project description, this prototype:

- Treats the ECC signing key as living behind the existing Ripple
  Custody Application (mocked here as a blackbox).
- Holds the ML-DSA-44 key in a parallel keystore module where a real
  PQ-capable HSM would plug in (mocked here as a blackbox of the same
  shape).
- Constructs a sample XRPL Payment, signs it hybrid-style entirely on
  the operator's machine, and submits the result to a hybrid-aware
  rippled (the standalone-mode demo or the multi-validator DevNet).

This document grows as later commits land the four subcommands, the
two mock modules, and the end-to-end reproduction script.

## Module boundary: the ECC custody mock

`EccCustodyMock` (`ecc_custody_mock.{h,cpp}`) stands in for the Ripple
Custody Application. It exposes three operations to the wallet:

- `initializeFromSeed(seed, key_type)` — create fresh custody material.
- `publicKey()` — return the public half so the wallet can compute
  account IDs and embed `sfSigningPubKey` on signed transactions.
- `signWithECC(payload)` — the blackbox sign request. In production
  this is a network call to the MPC+HSM-backed Custody service; in
  this PoC it loads its locally-persisted secret and calls
  `xrpl::sign` in-process.

The mock persists its state to a file separate from the wallet's
public state file (defaults to `./pq-wallet.custody.json` alongside
`./pq-wallet.json`). The wallet binary never opens that file
directly; the boundary is enforced at the filesystem level so the
swap-for-real seam stays honest.

Setting `RIPPLE_CUSTODY_FAIL=1` in the environment makes the mock
return a deliberately-corrupted signature, so reviewers can confirm
that the server-side hybrid binding gate is what makes the wallet's
acceptance non-trivial.

## Module boundary: the PQ keystore mock

`PqKeystoreMock` (`pq_keystore_mock.{h,cpp}`) is the symmetric module
on the PQ side, standing in for the "parallel key path that could
later be backed by a PQ-capable module" the PoC project description asks for.
It exposes the same shape as the ECC custody mock — `initializeFromPqSeed`,
`publicKey`, `signWithPq` — so the production two-key custody model is
visible in the code structure.

Persistence stores only the 32-byte PQ seed; `pqKeypair` deterministically
expands it into the full 1,440-byte public and 2,448-byte secret key on
load. State lives in its own file (defaults to `./pq-wallet.pq.json`
alongside `./pq-wallet.json`), with the same wallet-binary-never-opens-it
discipline.

Setting `PQ_KEYSTORE_FAIL=1` in the environment makes the PQ side of
the signing path return a deliberately-corrupted signature, mirroring
the `RIPPLE_CUSTODY_FAIL` hook for the ECC side.

## Build

The tool is gated behind an opt-in CMake option so the default
rippled build is untouched:

```sh
cmake -Dpq_wallet_cli=ON --preset=conan-debug
cmake --build .build --target pq-wallet-cli -j$(nproc)
```

The binary lands at `.build/pq-wallet-cli`.

## Usage

```sh
.build/pq-wallet-cli --help
```

Subcommand-level documentation, the run-demo script, and the
conceptual mapping to Ripple Custody Architecture follow in later
commits.
