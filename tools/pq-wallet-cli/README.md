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
