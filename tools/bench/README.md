# Hybrid PQC benchmark harness

Measures the cost of the hybrid ECC + ML-DSA-44 signature path on xrpld. One
command builds the binaries, stands up the DevNet, drives load, and writes a
per-configuration report under `results/<label>/`. The harness is pure Python
standard library (no third-party packages). See "Reading the report" below for
how to interpret each section.

## What is measured, and where

| Surface           | Measures                                             | Implemented in                         |
| ----------------- | ---------------------------------------------------- | -------------------------------------- |
| Microbench        | Raw crypto cost (sign/verify/keygen) and sizes       | C++ `xrpl.bench.pqc`                   |
| In-process probes | Verify cost inside a running node, at the real sites | C++ `BenchProbe` -> PerfLog `perf.log` |
| System level      | Throughput, latency, CPU/RAM, on-disk ledger growth  | this directory                         |

Absolute values (tps, CPU%) are workstation-specific and only **directional**;
read the ECC-vs-hybrid ratio, not the raw figure. Sizes and microbench timings
are exact; the verify probes are the cleanest in-node signal.

## Architecture

`run-suite.sh` is the orchestrator for one configuration. It chains the Python
tools below into five stages; `run-sweep.sh` just loops it over every profile and
repeat and packs a tarball. The per-file roles are in "The scripts" below; this is
how they fit together:

```
run-suite.sh  (one profile, end to end)
  build         xrpl.bench.pqc (C + AVX2)  ->  merge_microbench.py  ->  microbench.json
  stand up      scripts/devnet/setup.sh + docker compose up  (5 validators + 3 stock)
  prepare       prepare_accounts.py        (mint + fund + opt-in the account pool)
  presign       presign_corpus.py          (sign the blobs OFF the timed path)
  drive         rate_ramp_driver.py (disk snapshot per ramp level)  ||  resource_monitor.py
  aggregate     aggregate.py               ->  results/<label>/report.md  + completeness check
```

The workload in the drive stage is swappable: the default is single-sign
throughput, `--multisign` swaps in `multisign_probe.py` (per-signer verify
N-curve), `--ms-throughput` swaps in `presign_multisign.py` + the rate-ramp on
multi-signed traffic. Everything reads the node only through its JSON-RPC, and the
corpus is signed by the node's own `sign` RPC, so the same code path produces
ECC-only or hybrid blobs depending only on whether a `pq_seed_hex` is passed.

Two design points worth knowing before reading the code:

- **The probes live in a patch, not the hybrid branch.** `patches/develop-probes.patch`
  adds the `BenchProbe` call sites (`checkSign.single`, `checkSign.multi.per_signer`,
  `validation.verify`, `tx.deserialize`, and the ledger/persist sites). It is
  additive and no-op-safe, and it is compiled into all three profile binaries,
  including the clean-`develop` upstream baseline. That is why the upstream and
  hybrid verify numbers are directly comparable: they come from the same instrumented
  call sites.
- **Signing never happens on the timed path.** `prepare_accounts.py` and the
  `presign_*` tools do all minting, funding, opt-in, and signing up front. The timed
  drive only calls `submit`, so the throughput/latency numbers measure
  submit-to-validation.

## Prerequisites

- Built `.build/xrpld` and `.build/xrpl.bench.pqc`, and Docker (the DevNet runs as 8
  containers). Full from-scratch build steps are under "Running on a fresh machine".
- The `upstream` and `--avx2` profiles auto-build their own binaries on first use.

## Running

From the repository root:

```bash
# The three headline configurations:
tools/bench/run-suite.sh --profile upstream --label upstream
tools/bench/run-suite.sh --profile amendment-off --label amendment-off
tools/bench/run-suite.sh --profile hybrid --label hybrid

# Variants (hybrid only):
tools/bench/run-suite.sh --profile hybrid --avx2 --label hybrid-avx2            # node on the AVX2 ML-DSA backend
tools/bench/run-suite.sh --profile hybrid --multisign --label multisign         # per-signer verify cost across N
tools/bench/run-suite.sh --profile hybrid --ms-throughput --label ms-throughput # multi-sign throughput across N
```

`--smoke` runs a tiny end-to-end pass to check the setup.

Single-sign throughput is measured by the open-loop rate-ramp, which submits from a
random sender at a rising target rate and stops at the saturation knee. The full
statistical sweep across every configuration, with three repeats each, is one
command:

```bash
tools/bench/run-sweep.sh --repeats 3
```

It packs the results into `/tmp/bench-results.tgz` at the end. Running the whole
sweep on a fresh remote machine, from installing dependencies through collecting the
results, is covered in "Running on a fresh machine" at the end of this document.

Useful `run-suite.sh` knobs: `--pool` (account pool), `--rate-start` / `--rate-step`
/ `--rate-cap` (the rate band), `--rate-expected` (sizes the pre-signed corpus),
`--hold` (seconds per level).

### The three profiles

A profile selects which binary runs and how the network is configured. The three
exist so that each cost can be attributed to a single cause by changing only one
variable at a time.

| Profile         | Binary          | Quantum amendment | Transactions | What it is for                  |
| --------------- | --------------- | ----------------- | ------------ | ------------------------------- |
| `upstream`      | clean `develop` | absent            | ECC only     | pure-upstream reference         |
| `amendment-off` | `.build/xrpld`  | off               | ECC only     | modified binary on the ECC path |
| `hybrid`        | `.build/xrpld`  | on                | hybrid       | the post-quantum path           |

- **`upstream`** runs the unmodified upstream `develop` node (plus the no-op probe
  patch) on ECC-only traffic. It is the reference point: "what does the network
  do before any of our changes." Comparing it to `amendment-off` answers _did the
  modified binary slow the existing ECC path?_ (it should not).
- **`amendment-off`** runs our modified binary but with the Quantum amendment
  disabled, so transactions are still ECC-only. It isolates the cost of merely
  carrying the hybrid code from the cost of actually using it.
- **`hybrid`** is the full post-quantum path: the amendment is on and every source
  account is opted in, so transactions carry the ECC + ML-DSA-44 signatures.

The cleanest single number is `amendment-off` vs `hybrid`: same binary, same
network, the _only_ difference is whether transactions carry the PQ fields, so the
delta is the true per-transaction cost of going post-quantum.

## Workloads (what each run measures)

A profile sets up the network; a workload decides what traffic is driven through
it. The default workload is single-sign throughput; the flags below swap it for a
different question.

- **(default) single-sign throughput.** Drives ordinary single-signed Payments in
  an open-loop rate-ramp (see "Throughput method" below) and finds the saturation
  knee. Also captures in-process verify overhead, CPU/RAM, and on-disk ledger
  growth. Answers: _how many transactions per second can the network sustain, and
  what is the verify/resource/storage cost per transaction?_

- **`--avx2`.** Same as the default workload, but the node runs on the AVX2 build
  of ML-DSA (hardware-accelerated signature math) instead of the portable C
  build. Answers: _how much of the hybrid verify cost is the software backend?_
  (Throughput is unchanged because it is size-bound, not compute-bound; only the
  verify-overhead numbers move.)

- **`--multisign` -- the per-signer verify N-curve.** A multi-signed transaction
  is authorized by _N_ different signers, each adding its own signature. Under the
  hybrid scheme each signer adds an ECC **and** an ML-DSA signature, so the node
  performs one extra hybrid verify per signer. This workload submits N-of-N hybrid
  multi-signed transactions for several values of N (1, 4, 8, 16, 32) and times
  **one signer's verify** in isolation (the `checkSign.multi.per_signer` probe).
  The "N-curve" is how that per-signer cost behaves as N grows -- it is flat
  (~one ECC + one ML-DSA verify each), so the total verify cost of a transaction
  is simply N x that. Answers: _what does each additional signer add to
  verification cost?_

- **`--ms-throughput` -- multi-sign throughput by N.** The orthogonal question to
  the one above. Instead of timing one signer's verify, it runs a full open-loop
  rate-ramp using a corpus of N-of-N multi-signed transactions, for several N, and
  reports the max sustained transactions per second at each N. Because each signer
  adds ~3.7 KB to the transaction, larger quorums mean bigger transactions and
  fewer fit in a ledger, so throughput falls steeply as N grows. Answers: _how many
  multi-signed transactions per second can the network sustain as the signer count
  grows?_ (Each N gets its own rate band, centred on the expected knee, since a
  bigger transaction saturates the byte budget at a lower rate.)

## Regenerating all the numbers

The simplest path is `run-sweep.sh --repeats 3`, which runs every configuration in
order (the open-loop rate-ramp for the throughput configs, plus the per-signer verify
N-curve for multi-sign, both hybrid and an ECC baseline), writes a
`results/<label>/report.md` per run, and packs them at the end. The rest of this
section lists the individual runs behind it, for when you want to refresh one.

Each `run-suite.sh` invocation writes its own `results/<label>/report.md`. The
per-config commands are in "Running" above, plus the second AVX2 pass
(`--profile amendment-off --avx2`); run them one at a time, since they share ports.
Each one covers:

| Run                                 | What it measures                                    |
| ----------------------------------- | --------------------------------------------------- |
| `upstream`                          | ECC throughput/verify baseline                      |
| `amendment-off`                     | ECC-path regression, ECC verify, ECC disk baseline  |
| `hybrid`                            | hybrid throughput, verify, resources, disk          |
| `hybrid-avx2`, `amendment-off-avx2` | AVX2 verify overhead                                |
| `multisign` / `multisign-ecc`       | per-signer verify N-curve (hybrid and ECC baseline) |
| `ms-throughput`                     | multi-sign throughput by N (not in the sweep)       |

The crypto microbench and signature/payload sizes come out of every run, and each
run's `report.md` records the machine it ran on from `fingerprint.json`.

Notes:

- If a run dies mid-way it can leave the DevNet up; clear it before retrying with
  `docker compose -f scripts/devnet/docker-compose.yml down -v`.
- The `--ms-throughput` run pre-signs a large multi-signed corpus before each N
  (one `sign_for` per signer, ECC + ML-DSA each), so its setup is the slow part;
  the ramp itself is quick.

## The DevNet (`scripts/devnet/`)

`docker-compose.yml` runs 5 validators + 3 stock nodes on one host, each pinned
to disjoint cores (`cpuset`) so they do not contend. The **stock nodes are the
client-facing surface** the driver talks to:

| Node           | Admin RPC | Admin WS | cpuset (default) |
| -------------- | --------- | -------- | ---------------- |
| stock-1        | `5005`    | `6006`   | `10`             |
| stock-2        | `5006`    | `6007`   | `11`             |
| stock-3        | `5007`    | `6008`   | `12`             |
| validators 1-5 | (none)    | (none)   | `0,1`..`8,9`     |

**The default pinning needs >=16 logical CPUs** (highest pinned core is 12, with
13-15 left for the driver/monitor/OS). It adapts automatically: `setup.sh` writes
a `scripts/devnet/.env` that Compose auto-loads, and on a host with fewer than 16
logical CPUs it points every node at the full core range (`DEVNET_CPUSET_*`),
disabling exclusive pinning so the run still works -- the absolute numbers are
just less isolated, and the run's `fingerprint.json` records which mode was used.
Both `run-suite.sh` and the manual `setup.sh && docker compose up` path get this;
only a bare `docker compose up` with no prior `setup.sh` on a small host would
error on a cpuset referencing a missing core.

`setup.sh` generates per-node configs (fixed validator identities, so re-runs are
reproducible). Each node bind-mounts `scripts/devnet/perf/<node>/` so the
aggregator can read its PerfLog probe events. `down -v` wipes the data volumes,
so every run starts from an empty ledger (what makes the disk-growth delta
attributable to a single run). `run-suite.sh` handles setup and teardown; to
drive it by hand: `PROFILE=hybrid scripts/devnet/setup.sh && docker compose -f
scripts/devnet/docker-compose.yml up -d --build`.

## The scripts

| Script                 | Role                                                                       |
| ---------------------- | -------------------------------------------------------------------------- |
| `bench_lib.py`         | Shared RPC helpers + `LedgerPoller` (times when each tx hash validates)    |
| `prepare_accounts.py`  | Mints/funds the source-account pool; hybrid adds PQ keys + opt-in          |
| `presign_corpus.py`    | Pre-signs single-sign Payments so the timed run pays only submit->validate |
| `presign_multisign.py` | Sets SignerLists and pre-signs N-of-N multi-signed Payments                |
| `rate_ramp_driver.py`  | The throughput engine (open-loop rate-ramp; see below)                     |
| `run-sweep.sh`         | Runs the full configuration sweep and packs the results                    |
| `multisign_probe.py`   | Drives multi-signed txs and reads the per-signer verify probe              |
| `measure_disk.py`      | `du` of each node's database dir via `docker exec`; called per ramp level  |
| `resource_monitor.py`  | Samples `/proc` CPU/RSS + JobQueue backlog every 100 ms                    |
| `merge_microbench.py`  | Merges the C and AVX2 microbench JSON, taking the median aggregate         |
| `aggregate.py`         | Turns every artifact into `report.md`                                      |
| `run-suite.sh`         | Orchestrates all of the above end to end                                   |

**Throughput method.** The open-loop rate-ramp (`rate_ramp_driver.py`) submits at a
fixed target rate from a random sender out of a large account pool, ramping the rate
level by level, and stops at the knee where achieved throughput stops tracking the
offered rate. Spreading each submission across the pool keeps every account far below
its sequence and queue limits, and the DevNet runs a widened open ledger so
transactions enter ledgers instead of queueing. This drives the network to real
saturation rather than a per-account limit. Each account is pinned to one node so its
sequence stream stays ordered, and its pre-signed blobs are consumed in order; only
the choice of which account submits next is random. The multi-sign throughput runs
use the same rate-ramp, with a per-signer-count rate band.

## Outputs (`results/<label>/`)

| File                                        | Contents                                                   |
| ------------------------------------------- | ---------------------------------------------------------- |
| `report.md`                                 | The aggregated, human-readable report                      |
| `microbench.json` / `sizes.json`            | Crypto timings (both backends) + signature/payload sizes   |
| `fingerprint.json`                          | Machine/build provenance                                   |
| `run.cl_summary.json` / `run.cl_levels.csv` | Throughput/latency per rate level + saturation knee        |
| `monitor.csv`                               | Per-node CPU/RSS/backlog time series                       |
| `disk_before.json` / `run.cl_disk.json`     | Per-node DB size: pre-drive baseline, then each ramp level |
| `multisign.json`                            | Per-signer verify N-curve (`--multisign`)                  |
| `run_ms<N>.cl_summary.json`                 | Throughput per signer count (`--ms-throughput`)            |
| `corpus*.jsonl`                             | Pre-signed blobs (large; gitignored)                       |

The PerfLog probe events themselves are at `scripts/devnet/perf/<node>/perf.log`
(JSON lines of `event` / `duration_us`), pooled across nodes by the aggregator.

## Reading the report

- **Microbench** -- isolated crypto cost per op (ECC vs ML-DSA C vs AVX2).
- **In-process verify overhead** -- median + bootstrap 95% CI + p95/p99 per probe
  in a live node. The strongest, best-isolated metric.
- **Throughput** -- the rate-ramp knee: the highest rate the network keeps up with
  before achieved throughput stops tracking the offered rate. AVX2 leaves it
  unchanged and `job_backlog` stays at **zero** up to the knee, so throughput is
  bound by transaction and ledger **size**, not verify
  compute. That is the central finding.
- **Multi-sign** -- per-signer verify is flat (~ECC + ML-DSA verify) so total
  scales linearly in N; throughput falls ~1/N as the transaction grows.
- **Resource usage** -- RSS is the clean signal; CPU is a bursty peak on a shared
  host, not a sustained figure.
- **Ledger storage growth** -- bytes persisted per validated transaction
  (transaction + metadata + nodestore), read from the last ramp level whose
  snapshot is healthy so the figure survives a saturation-induced node collapse.

## Running on a fresh machine

The sections above are the reference; this is the from-bare-Linux-box runbook for the
full benchmark. Re-run the whole set on the target box rather than comparing its
numbers to another machine's.

### Machine requirements

| Resource     | Recommended                        | Notes                                                                                                                               |
| ------------ | ---------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| Logical CPUs | 16 or more                         | The DevNet pins 8 nodes to disjoint cores. Below 16, `setup.sh` relaxes the pinning automatically, so it still runs, less isolated. |
| RAM          | 64 GB or more                      | Eight nodes; resident set peaks near 11 GB per node under load.                                                                     |
| Disk         | 100 GB or more free                | Ledger data, the pre-signed corpus, and Docker images.                                                                              |
| OS           | Debian 12/13 or Ubuntu 22.04/24.04 | A recent glibc is required (see the note below).                                                                                    |

The reference run used an AMD EPYC 9454P (96 logical cores), 283 GB RAM, Debian 13,
glibc 2.41, recorded in every result's `fingerprint.json`.

glibc note: each DevNet node runs the host-built `xrpld` inside a `debian:trixie-slim`
container (glibc 2.41), so the machine that compiles the binary must have glibc no
newer than 2.41. Current Debian 12/13 and Ubuntu 22.04/24.04 satisfy this, and building
on the benchmark machine itself always does.

### Install dependencies

On Debian or Ubuntu, as a user with sudo:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    git ca-certificates curl python3 python3-pip python3-venv \
    build-essential g++ cmake pkg-config

# Docker Engine + the Compose v2 plugin.
sudo apt-get install -y docker.io docker-compose-v2
sudo systemctl enable --now docker
sudo usermod -aG docker "$USER" # log out/in (or `newgrp docker`) for this to take effect
```

Toolchain floor (newer is fine): GCC 12+ (or Clang 16+), CMake 3.22+, Python 3.11+,
Conan 2.17+. Install Conan in a venv to keep it out of system Python:

```bash
python3 -m venv ~/.venv/bench
. ~/.venv/bench/bin/activate
pip install --upgrade pip
pip install 'conan>=2.17'
```

Re-activate that venv in any new shell before building. The harness itself is pure
Python standard library and needs no packages.

### Get the source

```bash
git clone REPO_URL xrpld # REPO_URL is the repository's clone URL
cd xrpld
git checkout pqc/benchmarks
```

To build the exact same tree, check out the commit recorded in a result's
`fingerprint.json` (`git_commit`) instead of the branch tip.

### Build the node and the microbench

The harness expects the build under `.build/` (`.build/xrpld`, `.build/xrpl.bench.pqc`):

```bash
# One-time Conan setup: import the project profile and put the XRPLF remote above
# Conan Center so the patched recipes win.
conan config install conan/profiles/ -tf "$(conan config home)/profiles/"
conan remote add --index 0 --force xrplf https://conan.ripplex.io

conan install . --output-folder .build --build missing \
    --settings build_type=Release -o '&:xrpld=True' -o '&:bench=True'

cmake -S . -B .build \
    -DCMAKE_TOOLCHAIN_FILE=.build/build/generators/conan_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release -Dxrpld=ON -Dbench=ON

cmake --build .build --target xrpld xrpl.bench.pqc -j"$(nproc)"
```

The first build compiles every dependency and takes tens of minutes. The `upstream`
baseline and `--avx2` variants build their own binaries on first use
(`.build-develop/xrpld`, `.build-avx2/xrpld`), so no extra manual step is needed.

#### Optional: build elsewhere, copy the binaries

Building on the benchmark machine is the recommended path. To keep the toolchain off
the benchmark box, build on another machine and copy only the binaries to `.build/xrpld`
and `.build/xrpl.bench.pqc`; `run-suite.sh` reuses them when no build tree is present.
Both link just libc/libm, so this works as long as the benchmark box's glibc is the same
or newer than the build machine's (glibc is backward compatible). The `upstream` and
`--avx2` profiles compile their own binaries from the build tree, so a binary-only box
runs the `hybrid` and `amendment-off` C-only profiles out of the box; copy those extra
binaries too, or build on the box, if you also need the upstream baseline or the AVX2
numbers.

### Run, collect, and clean up

Smoke-test the box end to end first (`--profile hybrid --smoke`, see "Running" above),
then launch the full sweep. It is long: funding a 3000-account pool dominates each run,
so the n=3 sweep is several hours.

```bash
nohup bash tools/bench/run-sweep.sh --repeats 3 >/tmp/bench-sweep.log 2>&1 &
tail -f /tmp/bench-sweep.log # watch progress; the tail prints ALL_SWEEP_DONE when finished
```

It packs every `tools/bench/results/<label>/` directory (corpora excluded) into
`/tmp/bench-results.tgz`. See "Regenerating all the numbers" above for the per-config
breakdown when you only need to refresh one run. Copy the bundle off the box:

```bash
scp USER@HOST:/tmp/bench-results.tgz . && tar xzf bench-results.tgz
```

An interrupted run can leave the DevNet up; reset before retrying and tear down when
done:

```bash
docker compose -f scripts/devnet/docker-compose.yml down -v # stop nodes, wipe volumes
bash scripts/devnet/clean.sh                                # same, plus clears perf logs
```

### Troubleshooting

- **`conan install` cannot resolve a dependency (for example `ed25519`)**: the XRPLF
  remote is missing or below Conan Center. Re-run
  `conan remote add --index 0 --force xrplf https://conan.ripplex.io`.
- **CMake cannot find `benchmark`**: configured without the bench option. Re-run
  `conan install` with `-o '&:bench=True'` and `cmake` with `-Dbench=ON`.
- **`xrpld binary not found at .build/xrpld`** from `setup.sh`: the build did not
  complete, or you are not at the repository root.
- **A run aborts on a cpuset error on a small host**: run `bash scripts/devnet/setup.sh`
  once before `docker compose up`; it writes the relaxed core mapping (`run-suite.sh` and
  `run-sweep.sh` already do this).
- **`tefPAST_SEQ` lines in the `--ms-throughput` log**: expected at large signer counts;
  that workload is directional.
- **A retry reuses an old ledger / amendment voting falls back to 2 weeks**: stale Docker
  volumes. Always tear down with `down -v` between runs.
