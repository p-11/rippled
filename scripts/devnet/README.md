# Hybrid-PQ benchmark DevNet (5 validators + 3 stock nodes)

A self-contained Docker Compose setup that runs a private XRPL network where every validator signs with both an ECC ephemeral key and an ML-DSA-44 ephemeral key. Three stock (non-validating) nodes provide the client-facing submit/query surface for the benchmark harness.

## Prerequisites

- A locally built `xrpld` binary at `.build/xrpld` (repo root). Build with:
  ```
  cmake --build .build --target xrpld -j$(nproc)
  ```
- Docker + `docker compose` v2.
- 16+ logical cores for the default core pinning. On a smaller host `setup.sh`
  writes a `.env` that disables exclusive pinning (every node gets the full core
  range) so it still runs -- just with less isolation. Run `setup.sh` before
  `docker compose up`; a bare `docker compose up` on a small host with no prior
  `setup.sh` would error on a cpuset referencing a missing core.

## One-shot setup

```
bash scripts/devnet/setup.sh
docker compose -f scripts/devnet/docker-compose.yml up --build
```

`setup.sh` calls `xrpld --generate-hybrid-validator-token` five times (deterministic master PQ seeds, fresh master ECC seeds) and writes per-node `xrpld.cfg` + a shared `validators.txt` into `scripts/devnet/configs/`, plus per-node perf directories under `scripts/devnet/perf/`, and a `.env` with the host's core-pinning choice. All three are gitignored.

### Benchmark configurations

`setup.sh` honours a `PROFILE` environment variable:

- `hybrid` (default): Quantum pre-enabled, `pq_validations = fail_closed`.
- `amendment-off`: same hybrid binary, Quantum left disabled, no PQ validations. Isolates amendment-gating overhead from hybrid-path overhead.
- `upstream`: ECC-only sections, intended to be paired with a clean `develop` binary via `XRPLD_BIN` so the image carries the upstream baseline build.

```
PROFILE=amendment-off bash scripts/devnet/setup.sh
XRPLD_BIN=.build-develop/xrpld PROFILE=upstream bash scripts/devnet/setup.sh
docker compose -f scripts/devnet/docker-compose.yml up --build
```

## Client endpoints

| Node    | Admin RPC      | Admin WS       |
| ------- | -------------- | -------------- |
| stock-1 | 127.0.0.1:5005 | 127.0.0.1:6006 |
| stock-2 | 127.0.0.1:5006 | 127.0.0.1:6007 |
| stock-3 | 127.0.0.1:5007 | 127.0.0.1:6008 |

The load driver can spread submissions across all three.

## Smoke-test the network

```
# After the network is up (~20 s for the validators to peer and the
# Quantum amendment to activate), run the bootstrap script:
python3 scripts/devnet/bootstrap.py
```

It mints a hybrid keypair via `wallet_propose`, funds an account from the genesis master, opts the account in to hybrid via `AccountSet asfQuantum`, sends a follow-up hybrid Payment, and asserts that `account_info` shows `sfQuantumPubKey` on the AccountRoot. (Run only under the default `hybrid` profile.)

## Layout

```
scripts/devnet/
├── Dockerfile              # debian:trixie-slim + xrpld binary (XRPLD_BIN arg)
├── docker-compose.yml      # 5 validators + 3 stock nodes, cpuset-pinned
├── setup.sh                # generate configs (PROFILE-aware)
├── clean.sh                # tear down + wipe volumes and perf logs
├── bootstrap.py            # end-to-end smoke test
├── templates/
│   ├── xrpld.cfg.tmpl    # validator nodes
│   └── stock.cfg.tmpl      # stock (client-facing) nodes
├── configs/                # generated (gitignored)
└── perf/                   # generated PerfLog output, host-mounted (gitignored)
```

## Notes

- The genesis account (`rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh`, secret `masterpassphrase`) holds all initial XRP; bootstrap.py funds test accounts from it.
- Hybrid Validators sign Manifests + Validations + Consensus proposals four ways (ECC ephemeral, ECC master, PQ ephemeral, PQ master). Under the `hybrid` profile, `pq_validations = fail_closed` drops any peer validation that lacks a PQ signature when the validator's manifest declares one.
- Each node enables the `[perf]` section; PerfLog writes JobQueue events and the in-process verification probes to `scripts/devnet/perf/<node>/perf.log`, which the aggregator consumes.
- Each node config carries a large `[transaction_queue]` (a high `target_txn_in_ledger` and `maximum_txn_in_ledger`). This widens the open ledger so that under load transactions enter the ledger instead of queueing, which is what lets the open-loop rate-ramp drive the network to real saturation rather than to a queue limit. It does not change consensus or validation; it only raises how many transactions a single ledger will admit.
- Resetting the network: `bash scripts/devnet/clean.sh` (wraps `docker compose down -v` and clears `perf/`).
