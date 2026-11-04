#!/usr/bin/env bash
# One-command benchmark reproduction for a single configuration.
#
# Usage:
#   tools/bench/run-suite.sh [--profile hybrid|amendment-off|upstream]
#                            [--label NAME] [--smoke] [--avx2] [--multisign]
#                            [--method steady|ramp] [--pool N|auto]
#                            [--rate-start N] [--rate-step N] [--rate-cap N]
#                            [--rates R1,R2,...] [--rate-expected N]
#                            [--ramp S] [--settle S] [--measure S] [--drain S]
#                            [--max-interval S] [--fee DROPS] [--node-size SIZE]
#                            [--gen-procs N] [--gen-threads N]
#
# --method steady (default) is the methodology from the RippleX review
#   (Appendix A): xrpld's default transaction-queue settings, ramp to each load,
#   hold it, score only a steady-state window, pass while every validated-ledger
#   interval stays <= --max-interval (5 s), throughput read from the ledgers.
#   --method ramp reproduces the Phase 2 rate-ramp driver unchanged.
# --pool auto sizes the account pool so no account submits twice within one
#   ledger at the top rate: rate_expected x max_interval x 1.25.
# --rate-expected sizes the pre-signed corpus: every level up to it (plus two
#   steps) is covered; a run that passes beyond it stops as generator-limited.
#
# Load generator placement (steady method):
#   GEN_SSH=user@host  run the driver on another machine over ssh (python3 needed
#                      there; tools/bench/*.py and the corpus are rsync'd over and
#                      the raw outputs pulled back)
#   NODE_HOST=IP       address of this DevNet host as seen by the generator
#                      (default 127.0.0.1); RPC ports 5005-5007 (stock) and
#                      5010-5014 (validators) must be reachable from it
# Keep both hosts NTP-synced: the generator's timestamps are lined up with disk
# snapshots taken here.
#
# --avx2 runs the whole node on the AVX2 ML-DSA backend (not just the microbench).
# --multisign swaps the throughput ramp for the multi-sign per-signer verify probe
#   across N (hybrid measures hybrid per-signer verify, ECC-path profiles the ECC one).
# --ms-throughput swaps it for an open-loop rate-ramp on multi-signed traffic, one
#   ramp per N, measuring tps and the knee.
# --ms-n N1,N2,... overrides the signer counts (default 4,8,16,32).
#
# For PROFILE=upstream, point XRPLD_BIN at a binary built from a clean develop tree
# (the upstream ECC baseline); the default uses the hybrid build at .build/xrpld.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BENCH_DIR="${REPO_ROOT}/tools/bench"
DEVNET_DIR="${REPO_ROOT}/scripts/devnet"
COMPOSE="${DEVNET_DIR}/docker-compose.yml"

PROFILE="hybrid"
LABEL=""
POOL=1000
POOL_SET=0
METHOD="steady"
RATES=""
RAMP=30
SETTLE=60
MEASURE=120
DRAIN=30
MAX_INTERVAL=5
GEN_PROCS=4
GEN_THREADS=48
STEADY_FEE=1000000 # high enough that open-ledger fee escalation does not bind; same for every config
NODE_SIZE="${NODE_SIZE:-}"
NODE_HOST="${NODE_HOST:-127.0.0.1}"
GEN_SSH="${GEN_SSH:-}"
GEN_DIR="${GEN_DIR:-/tmp/xrpl-bench-gen}"
HOLD=12
WARMUP=6
SMOKE=0
REBUILD_UPSTREAM=0
REBUILD_AVX2=0
AVX2_NODE=0
MULTISIGN=0
MS_THROUGHPUT=0
MS_N_VALUES="4,8,16,32"
# Multi-sign throughput saturates ~1/N (a tx grows ~3.7 KB per signer and the
# network is byte-bound), so each N gets a rate band centred on MS_TP_BASE/N tx/s
# and a payer pool sized from MS_SAT_BASE/N.
MS_SAT_BASE=900
MS_TP_BASE=1000
# Same value across profiles gives matched sample sizes for the ECC vs hybrid
# comparison (probe-event count is deterministic in this).
MS_TARGET_EVENTS=6000
RATE_START=500
RATE_STEP=250
RATE_CAP=4000      # safety ceiling for the open-ended ramp
RATE_EXPECTED=3000 # size the pre-signed corpus to cover up to this rate
CORPUS_FEE=50000
SIGN_THREADS=24
URL="http://127.0.0.1:5005"
# The driver round-robins submits across the three stock nodes, account-pinned so
# each account's Sequence stream stays ordered.
DRIVER_URLS="http://127.0.0.1:5005,http://127.0.0.1:5006,http://127.0.0.1:5007"
export XRPLD_BIN="${XRPLD_BIN:-.build/xrpld}"

AVX2_BUILD="${REPO_ROOT}/.build-avx2"

UPSTREAM_BIN_REL=".build-develop/xrpld"
UPSTREAM_WORKTREE="${UPSTREAM_WORKTREE:-${REPO_ROOT}/.upstream-baseline/develop-probed}"
UPSTREAM_PATCH="${BENCH_DIR}/patches/develop-probes.patch"
UPSTREAM_BASE_REF="${UPSTREAM_BASE_REF:-develop}"

# Fresh detached worktree at the develop base with the probe patch applied: the
# patch is the two additive, no-op-safe probe commits, so the result is upstream
# develop plus the verification probes and nothing else.
build_upstream_binary() {
    echo "=== building upstream baseline (${UPSTREAM_BASE_REF} + probes) ==="
    mkdir -p "${REPO_ROOT}/.build-develop"
    if git -C "${REPO_ROOT}" worktree list --porcelain | grep -qF "${UPSTREAM_WORKTREE}"; then
        git -C "${REPO_ROOT}" worktree remove --force "${UPSTREAM_WORKTREE}" || true
    fi
    rm -rf "${UPSTREAM_WORKTREE}"
    mkdir -p "$(dirname "${UPSTREAM_WORKTREE}")"
    git -C "${REPO_ROOT}" worktree add --detach "${UPSTREAM_WORKTREE}" "${UPSTREAM_BASE_REF}"
    git -C "${UPSTREAM_WORKTREE}" apply --3way "${UPSTREAM_PATCH}"
    (
        cd "${UPSTREAM_WORKTREE}"
        conan install . --output-folder=.build --build=missing -s build_type=Release \
            -o '&:xrpld=True'
        cmake -S . -B .build \
            -DCMAKE_TOOLCHAIN_FILE=.build/build/generators/conan_toolchain.cmake \
            -DCMAKE_BUILD_TYPE=Release
        cmake --build .build --target xrpld -j"$(nproc)"
    )
    cp "${UPSTREAM_WORKTREE}/.build/xrpld" "${REPO_ROOT}/${UPSTREAM_BIN_REL}"
    echo "upstream baseline binary ready at ${UPSTREAM_BIN_REL}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --profile)
            PROFILE="$2"
            shift 2
            ;;
        --label)
            LABEL="$2"
            shift 2
            ;;
        --pool)
            POOL="$2"
            POOL_SET=1
            [[ "${POOL}" == "auto" ]] && POOL_SET=0 && POOL=1000
            shift 2
            ;;
        --method)
            METHOD="$2"
            shift 2
            ;;
        --rates)
            RATES="$2"
            shift 2
            ;;
        --ramp)
            RAMP="$2"
            shift 2
            ;;
        --settle)
            SETTLE="$2"
            shift 2
            ;;
        --measure)
            MEASURE="$2"
            shift 2
            ;;
        --drain)
            DRAIN="$2"
            shift 2
            ;;
        --max-interval)
            MAX_INTERVAL="$2"
            shift 2
            ;;
        --fee)
            STEADY_FEE="$2"
            shift 2
            ;;
        --node-size)
            NODE_SIZE="$2"
            shift 2
            ;;
        --gen-procs)
            GEN_PROCS="$2"
            shift 2
            ;;
        --gen-threads)
            GEN_THREADS="$2"
            shift 2
            ;;
        --rate-start)
            RATE_START="$2"
            shift 2
            ;;
        --rate-step)
            RATE_STEP="$2"
            shift 2
            ;;
        --rate-cap)
            RATE_CAP="$2"
            shift 2
            ;;
        --rate-expected)
            RATE_EXPECTED="$2"
            shift 2
            ;;
        --hold)
            HOLD="$2"
            shift 2
            ;;
        --warmup)
            WARMUP="$2"
            shift 2
            ;;
        --smoke)
            SMOKE=1
            shift
            ;;
        --rebuild-upstream)
            REBUILD_UPSTREAM=1
            shift
            ;;
        --rebuild-avx2)
            REBUILD_AVX2=1
            shift
            ;;
        --avx2)
            AVX2_NODE=1
            shift
            ;;
        --multisign)
            MULTISIGN=1
            shift
            ;;
        --ms-throughput)
            MS_THROUGHPUT=1
            shift
            ;;
        --ms-n)
            MS_N_VALUES="$2"
            shift 2
            ;;
        *)
            echo "unknown arg: $1" >&2
            exit 1
            ;;
    esac
done

if [[ ${SMOKE} -eq 1 ]]; then
    HOLD=5
    WARMUP=3
    if [[ ${MS_THROUGHPUT} -eq 1 ]]; then
        MS_N_VALUES="2,4"
        MS_SAT_BASE=60
        MS_TP_BASE=80
        POOL=70
    else
        POOL=20
        POOL_SET=1
        RAMP=5
        SETTLE=10
        MEASURE=20
        DRAIN=10
        GEN_PROCS=2
        GEN_THREADS=8
        # The multi-sign N-curve probe needs >=40 accounts (max N signers +
        # payers); the 20-account throughput pool is too small for it.
        [[ ${MULTISIGN} -eq 1 ]] && POOL=50
        RATE_START=200
        RATE_STEP=100
        RATE_CAP=400
        RATE_EXPECTED=300
    fi
fi

if [[ "${PROFILE}" == "upstream" ]]; then
    [[ "${XRPLD_BIN}" == ".build/xrpld" ]] && export XRPLD_BIN="${UPSTREAM_BIN_REL}"
    if [[ ${REBUILD_UPSTREAM} -eq 1 || ! -x "${REPO_ROOT}/${XRPLD_BIN}" ]]; then
        build_upstream_binary
    else
        echo "Reusing upstream binary at ${XRPLD_BIN} (pass --rebuild-upstream to rebuild)"
    fi
fi

# --avx2 has no effect on the upstream profile: upstream develop has no ML-DSA.
if [[ ${AVX2_NODE} -eq 1 ]]; then
    if [[ "${PROFILE}" == "upstream" ]]; then
        echo "Note: upstream profile uses develop (no ML-DSA); --avx2 has no effect."
    else
        export XRPLD_BIN=".build-avx2/xrpld"
        if [[ ${REBUILD_AVX2} -eq 1 || ! -x "${REPO_ROOT}/${XRPLD_BIN}" ]]; then
            echo "=== building AVX2 node binary (one-time) ==="
            cmake -S "${REPO_ROOT}" -B "${AVX2_BUILD}" \
                -DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/.build/build/generators/conan_toolchain.cmake" \
                -DCMAKE_BUILD_TYPE=Release -Dbench=ON -Dmldsa_avx2=ON
            cmake --build "${AVX2_BUILD}" --target xrpld -j"$(nproc)"
        else
            echo "Reusing AVX2 node binary at ${XRPLD_BIN}"
        fi
    fi
fi

LABEL="${LABEL:-${PROFILE}}"
OUT="${BENCH_DIR}/results/${LABEL}"
mkdir -p "${OUT}"
HYBRID_FLAG=""
[[ "${PROFILE}" == "hybrid" ]] && HYBRID_FLAG="--hybrid"
case "${METHOD}" in steady | ramp) ;; *)
    echo "unknown --method ${METHOD} (steady|ramp)" >&2
    exit 1
    ;;
esac
export NODE_SIZE

STEADY_URLS="http://${NODE_HOST}:5005,http://${NODE_HOST}:5006,http://${NODE_HOST}:5007"
VALIDATOR_URLS="http://${NODE_HOST}:5010,http://${NODE_HOST}:5011,http://${NODE_HOST}:5012,http://${NODE_HOST}:5013,http://${NODE_HOST}:5014"
if [[ "${METHOD}" == "steady" && ${MULTISIGN} -eq 0 && ${MS_THROUGHPUT} -eq 0 ]]; then
    # Levels the corpus must cover: explicit --rates, else start..rate_expected
    # plus two steps (so the first failing level above the expected knee runs).
    if [[ -n "${RATES}" ]]; then
        STEADY_RATES="${RATES}"
        CORPUS_RATES="${RATES}"
    else
        STEADY_RATES=$(python3 -c "print(','.join(str(r) for r in range(${RATE_START}, ${RATE_CAP}+1, ${RATE_STEP})))")
        CORPUS_RATES=$(python3 -c "print(','.join(str(r) for r in range(${RATE_START}, min(${RATE_CAP}, ${RATE_EXPECTED}+2*${RATE_STEP})+1, ${RATE_STEP})))")
    fi
    TOP_RATE=$(python3 -c "print(max([${CORPUS_RATES}]))")
    if [[ ${POOL_SET} -eq 0 ]]; then
        POOL=$(python3 -c "import math;print(math.ceil(${TOP_RATE}*${MAX_INTERVAL}*1.25))")
    fi
fi

if [[ ${MULTISIGN} -eq 1 ]]; then
    # The probe needs only a few dozen accounts; shrink the default pool so prep
    # stays quick unless the caller asked for a specific size.
    [[ ${POOL} -eq 1000 ]] && POOL=60
fi

if [[ ${MS_THROUGHPUT} -eq 1 ]]; then
    # The smallest N needs the most payers (MS_SAT_BASE/N); size the pool for it
    # plus the largest signer set.
    MAX_N=$(echo "${MS_N_VALUES}" | tr ',' '\n' | sort -n | tail -1)
    MIN_N=$(echo "${MS_N_VALUES}" | tr ',' '\n' | sort -n | head -1)
    [[ ${POOL} -eq 1000 ]] && POOL=$((MS_SAT_BASE / MIN_N + MAX_N + 8))
fi

echo "=== microbench: C-only and AVX2 ML-DSA backends ==="
# On a binary-only run host (prebuilt binaries rsync'd to a machine with no build
# tree, as we do to run at scale) reuse the existing binary; the bench binary links
# only libc/libm, so it is as portable as xrpld.
if [[ -f "${REPO_ROOT}/.build/CMakeCache.txt" ]]; then
    cmake --build "${REPO_ROOT}/.build" --target xrpl.bench.pqc -j"$(nproc)"
elif [[ ! -x "${REPO_ROOT}/.build/xrpl.bench.pqc" ]]; then
    echo "No build tree and no prebuilt .build/xrpl.bench.pqc to run the microbench." >&2
    exit 1
else
    echo "No build tree; reusing prebuilt .build/xrpl.bench.pqc"
fi
"${REPO_ROOT}/.build/xrpl.bench.pqc" --benchmark_format=json --benchmark_repetitions=10 --benchmark_report_aggregates_only=true >"${OUT}/raw-c.json" 2>/dev/null || true
# AVX2 backend reuses the main build's Conan toolchain (mldsa_avx2 is a CMake
# option, not a Conan one). merge_microbench accepts "-" for a missing one.
if [[ -f "${REPO_ROOT}/.build/CMakeCache.txt" ]] &&
    { [[ ${REBUILD_AVX2} -eq 1 ]] || [[ ! -x "${AVX2_BUILD}/xrpl.bench.pqc" ]]; }; then
    echo "building AVX2 microbench (one-time) ..."
    cmake -S "${REPO_ROOT}" -B "${AVX2_BUILD}" \
        -DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/.build/build/generators/conan_toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release -Dbench=ON -Dmldsa_avx2=ON
    cmake --build "${AVX2_BUILD}" --target xrpl.bench.pqc -j"$(nproc)"
fi
AVX2_RAW="-"
if [[ -x "${AVX2_BUILD}/xrpl.bench.pqc" ]]; then
    "${AVX2_BUILD}/xrpl.bench.pqc" --benchmark_format=json --benchmark_repetitions=10 --benchmark_report_aggregates_only=true >"${OUT}/raw-avx2.json" 2>/dev/null || true
    AVX2_RAW="${OUT}/raw-avx2.json"
else
    echo "No AVX2 microbench binary; reporting C-only timings"
fi
python3 "${BENCH_DIR}/merge_microbench.py" "${OUT}/raw-c.json" "${AVX2_RAW}" \
    "${OUT}/microbench.json"
cp "${OUT}/microbench.json" "${OUT}/sizes.json"

echo "=== reset + setup DevNet (PROFILE=${PROFILE}) ==="
bash "${DEVNET_DIR}/clean.sh" >/dev/null 2>&1 || true
PROFILE="${PROFILE}" XRPLD="${REPO_ROOT}/${XRPLD_BIN}" bash "${DEVNET_DIR}/setup.sh"

echo "=== environment fingerprint ==="
python3 - "${OUT}/fingerprint.json" "${PROFILE}" "${XRPLD_BIN}" <<'PY'
import json, platform, subprocess, sys

def cmd(args):
    try:
        return subprocess.run(args, capture_output=True, text=True).stdout.strip()
    except Exception:
        return ""

def cmake_var(name):
    try:
        for line in open(".build/CMakeCache.txt"):
            if line.startswith(name + ":"):
                return line.split("=", 1)[1].strip()
    except Exception:
        pass
    return ""

mem_kb = next((int(l.split()[1]) for l in open("/proc/meminfo")
               if l.startswith("MemTotal")), 0)

fp = {
    "profile": sys.argv[2],
    "xrpld_bin": sys.argv[3],
    "mldsa_node_backend": "avx2" if "build-avx2" in sys.argv[3] else "c-only",
    "cpu_model": next((l.split(":",1)[1].strip() for l in open("/proc/cpuinfo")
                       if l.startswith("model name")), ""),
    "cpu_count": __import__("os").cpu_count(),
    "mem_gib": round(mem_kb / 1024 / 1024, 1),
    "kernel": platform.release(),
    "glibc": " ".join(platform.libc_ver()),
    "compiler": (cmd(["cc", "--version"]).splitlines() or [""])[0],
    "build_type": cmake_var("CMAKE_BUILD_TYPE"),
    "docker": cmd(["docker", "--version"]),
    "git_commit": cmd(["git", "rev-parse", "HEAD"]),
    "mldsa_commit": cmd(["git", "-C", "external/mldsa-native", "rev-parse", "HEAD"]),
    "cpuset_note": next((l.split("=", 1)[1].strip().strip('"')
                         for l in open("scripts/devnet/.env")
                         if l.startswith("DEVNET_PINNING_NOTE=")), "unknown"),
}
json.dump(fp, open(sys.argv[1], "w"), indent=2)
print(json.dumps(fp, indent=2))
PY

python3 - "${OUT}/fingerprint.json" <<PY2
import json, sys
p = sys.argv[1]
fp = json.load(open(p))
fp.update({
    "method": "${METHOD}",
    "node_size": "${NODE_SIZE}" or "auto (xrpld default)",
    "transaction_queue": "xrpld defaults",
    "rates": "${STEADY_RATES:-}",
    "ramp_s": ${RAMP}, "settle_s": ${SETTLE}, "measure_s": ${MEASURE}, "drain_s": ${DRAIN},
    "max_interval_s": ${MAX_INTERVAL},
    "fee_drops": ${STEADY_FEE},
    "pool": ${POOL},
    "generator": "${GEN_SSH:-local}",
    "generator_procs": ${GEN_PROCS}, "generator_threads": ${GEN_THREADS},
    "node_host_from_generator": "${NODE_HOST}",
})
json.dump(fp, open(p, "w"), indent=2)
PY2

echo "=== bring up DevNet ==="
docker compose -f "${COMPOSE}" up -d --build

echo "=== wait for network + amendment ==="
python3 - "${URL}" "${PROFILE}" <<'PY'
import sys, time, urllib.request, json
url, profile = sys.argv[1], sys.argv[2]
def rpc(m, p):
    body = json.dumps({"method": m, "params": [p]}).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=10).read()).get("result", {})
for _ in range(120):
    try:
        if rpc("server_info", {}).get("info", {}).get("server_state") in ("full","proposing","tracking"):
            break
    except Exception:
        pass
    time.sleep(2)
if profile == "hybrid":
    for _ in range(120):
        r = rpc("feature", {"feature": "Quantum"})
        ams = {k: v for k, v in r.items() if isinstance(v, dict)}
        if next(iter(ams.values()), {}).get("enabled"):
            break
        time.sleep(4)
# Require the validated ledger to advance before handing off to the funder, so
# account_info "current" does not hit InsufficientNetworkMode on a node that has
# reported a steady state but is not yet serving the current ledger.
def validated():
    try:
        return int(rpc("ledger", {"ledger_index": "validated"}).get("ledger", {}).get("ledger_index", 0))
    except Exception:
        return 0
base = validated()
for _ in range(60):
    if validated() >= base + 3 and base > 0:
        break
    time.sleep(2)
print("network ready")
PY

cd "${BENCH_DIR}"
echo "=== prepare accounts ==="
PREP_FEE_ARGS=()
[[ "${METHOD}" == "steady" ]] && PREP_FEE_ARGS=(--fee "${STEADY_FEE}")
python3 prepare_accounts.py --url "${URL}" --pool-size "${POOL}" ${HYBRID_FLAG} \
    "${PREP_FEE_ARGS[@]}" --out "${OUT}/accounts.json"

if [[ ${MS_THROUGHPUT} -eq 1 ]]; then
    # Clear any prior run's per-N artifacts so a changed N list cannot leave a
    # stale run_msN that the aggregator would still pick up.
    rm -f "${OUT}"/run_ms*.cl_summary.json "${OUT}"/run_ms*.cl_levels.csv \
        "${OUT}"/corpus_ms*.jsonl
    for N in ${MS_N_VALUES//,/ }; do
        C=$((MS_TP_BASE / N))
        [[ ${C} -lt 20 ]] && C=20
        # Five levels bracketing the expected knee, with two above it so the
        # plateau (two non-tracking levels) is detectable.
        RATES=$(python3 -c "c=${C};print(','.join(str(int(c*f)) for f in (0.6,0.8,1.0,1.2,1.4)))")
        P=$((MS_SAT_BASE / N))
        [[ ${P} -lt 50 ]] && P=50
        # Corpus depth per payer to cover every level at HOLD seconds, +30% safety.
        K=$(python3 -c "import math;r=[${RATES}];print(math.ceil(sum(r)*${HOLD}/${P}*1.3)+5)")
        echo "=== multi-sign throughput N=${N} (payers ${P}, rates ${RATES}, per-account ${K}) ==="
        python3 presign_multisign.py --urls "${DRIVER_URLS}" \
            --accounts "${OUT}/accounts.json" --n "${N}" --payers "${P}" \
            --per-account "${K}" --threads "${SIGN_THREADS}" \
            --out "${OUT}/corpus_ms${N}.jsonl"
        python3 rate_ramp_driver.py --urls "${DRIVER_URLS}" \
            --corpus "${OUT}/corpus_ms${N}.jsonl" --rates "${RATES}" \
            --hold "${HOLD}" --warmup "${WARMUP}" --out "${OUT}/run_ms${N}"
    done
elif [[ ${MULTISIGN} -eq 1 ]]; then
    echo "=== multi-sign probe (N curve) ==="
    python3 multisign_probe.py --url "${URL}" --accounts "${OUT}/accounts.json" \
        --perf-dir "${DEVNET_DIR}/perf" --target-events "${MS_TARGET_EVENTS}" \
        --out "${OUT}/multisign.json"
elif [[ "${METHOD}" == "ramp" ]]; then
    # Open-loop rate-ramp: size the corpus to cover each account's share of every
    # level from start up to the expected knee (plus a warmup burst), +30% safety.
    PER_ACCOUNT=$(python3 -c "import math;rates=list(range(${RATE_START},${RATE_EXPECTED}+1,${RATE_STEP}));total=sum(rates)*${HOLD}+${RATE_START}*15;print(math.ceil(total/${POOL}*1.3))")
    echo "rate-ramp: per-account corpus depth=${PER_ACCOUNT} (pool ${POOL}, ${RATE_START}+${RATE_STEP}->~${RATE_EXPECTED} tps, hold ${HOLD}s)"
    echo "=== presign corpus (${PER_ACCOUNT}/account, ${SIGN_THREADS} threads) ==="
    python3 presign_corpus.py --urls "${DRIVER_URLS}" --accounts "${OUT}/accounts.json" \
        --per-account "${PER_ACCOUNT}" --fee "${CORPUS_FEE}" --threads "${SIGN_THREADS}" \
        --out "${OUT}/corpus.jsonl"

    echo "=== disk snapshot (before drive) ==="
    python3 measure_disk.py --out "${OUT}/disk_before.json" ||
        echo "WARNING: disk_before snapshot failed; storage growth will be unavailable"

    echo "=== open-loop rate-ramp drive + monitor ==="
    NLVL=$(python3 -c "print((${RATE_CAP}-${RATE_START})//${RATE_STEP}+1)")
    MON_DUR=$(python3 -c "print(int((${HOLD}+25)*(${NLVL}+2)))")
    python3 resource_monitor.py --url "${URL}" --duration "${MON_DUR}" --out "${OUT}/monitor.csv" &
    MON_PID=$!
    python3 rate_ramp_driver.py --urls "${DRIVER_URLS}" --corpus "${OUT}/corpus.jsonl" \
        --rate-start "${RATE_START}" --rate-step "${RATE_STEP}" --rate-cap "${RATE_CAP}" \
        --hold "${HOLD}" --warmup "${WARMUP}" --out "${OUT}/run"

    # Snapshot disk right after the drive, before waiting out the monitor's tail.
    # The driver over-runs are short, but the monitor's duration can idle for
    # minutes past the drive; taking the snapshot now keeps it close to the run and
    # avoids an idle window in which a node under disk pressure could exit and leave
    # the snapshot empty (which is exactly what cost a hybrid run its storage data).
    # A short settle first lets the last transactions flush to the nodestore.
    sleep 12
    echo "=== disk snapshot (after drive) ==="
    python3 measure_disk.py --out "${OUT}/disk_after.json" ||
        echo "WARNING: disk_after snapshot failed; storage growth unavailable for this run"

    wait "${MON_PID}" 2>/dev/null || true
else
    # Steady-state method (RippleX review, Appendix A).
    PER_ACCOUNT=$(python3 -c "
import math, sys
sys.path.insert(0, '.')
from steady_state_driver import corpus_needed
need = corpus_needed([${CORPUS_RATES}], ${RAMP}, ${SETTLE}, ${MEASURE})
print(math.ceil(need / ${POOL} * 1.1) + 2)")
    echo "steady-state: pool ${POOL}, levels ${STEADY_RATES} tx/s (corpus covers ${CORPUS_RATES}),"
    echo "  ramp ${RAMP}s settle ${SETTLE}s measure ${MEASURE}s, pass <= ${MAX_INTERVAL}s, fee ${STEADY_FEE} drops"
    echo "=== presign corpus (${PER_ACCOUNT}/account, ${SIGN_THREADS} threads) ==="
    python3 presign_corpus.py --urls "${DRIVER_URLS}" --accounts "${OUT}/accounts.json" \
        --per-account "${PER_ACCOUNT}" --fee "${STEADY_FEE}" --threads "${SIGN_THREADS}" \
        --out "${OUT}/corpus.jsonl"

    echo "=== disk snapshot (before drive) ==="
    python3 measure_disk.py --out "${OUT}/disk_before.json" ||
        echo "WARNING: disk_before snapshot failed; storage growth will be unavailable"

    NLVL=$(python3 -c "print(len([${STEADY_RATES}]))")
    MON_DUR=$(python3 -c "print(int((${RAMP}+${SETTLE}+${MEASURE})*${NLVL}+${DRAIN}+180))")
    python3 resource_monitor.py --url "${URL}" --duration "${MON_DUR}" --out "${OUT}/monitor.csv" &
    MON_PID=$!

    DRIVER_ARGS=(--urls "${STEADY_URLS}" --validator-urls "${VALIDATOR_URLS}"
        --rates "${STEADY_RATES}" --ramp "${RAMP}" --settle "${SETTLE}" --measure "${MEASURE}"
        --drain "${DRAIN}" --max-interval "${MAX_INTERVAL}"
        --procs "${GEN_PROCS}" --threads "${GEN_THREADS}")
    echo "=== steady-state drive ==="
    if [[ -n "${GEN_SSH}" ]]; then
        echo "generator on ${GEN_SSH}:${GEN_DIR}, nodes at ${NODE_HOST}"
        ssh "${GEN_SSH}" "rm -rf '${GEN_DIR}' && mkdir -p '${GEN_DIR}/out'"
        rsync -az "${BENCH_DIR}"/*.py "${GEN_SSH}:${GEN_DIR}/"
        rsync -az "${OUT}/corpus.jsonl" "${GEN_SSH}:${GEN_DIR}/corpus.jsonl"
        ssh "${GEN_SSH}" "uname -a; nproc; grep -m1 'model name' /proc/cpuinfo; python3 --version" \
            >"${OUT}/generator_host.txt" 2>&1 || true
        ssh "${GEN_SSH}" "cd '${GEN_DIR}' && python3 steady_state_driver.py --corpus corpus.jsonl \
            --out out/steady $(printf '%q ' "${DRIVER_ARGS[@]}")"
        rsync -az "${GEN_SSH}:${GEN_DIR}/out/" "${OUT}/"
    else
        echo "local generator (same host as the nodes)" >"${OUT}/generator_host.txt"
        python3 steady_state_driver.py --corpus "${OUT}/corpus.jsonl" --out "${OUT}/steady" \
            "${DRIVER_ARGS[@]}"
    fi

    echo "=== disk snapshot (after drive) ==="
    python3 measure_disk.py --out "${OUT}/disk_after.json" ||
        echo "WARNING: disk_after snapshot failed; storage growth unavailable for this run"
    echo "=== storage breakdown ==="
    python3 storage_breakdown.py --prefix "${OUT}/steady" \
        --disk-before "${OUT}/disk_before.json" --disk-after "${OUT}/disk_after.json" \
        --url "${URL}" --container rippled-stock-1 --accounts "${OUT}/accounts.json" \
        --out "${OUT}/storage" || echo "WARNING: storage breakdown failed"

    kill "${MON_PID}" 2>/dev/null || true
    wait "${MON_PID}" 2>/dev/null || true
fi

echo "=== aggregate ==="
cd "${REPO_ROOT}"
python3 "${BENCH_DIR}/aggregate.py" --perf-dir "${DEVNET_DIR}/perf" \
    --monitor "${OUT}/monitor.csv" --driver-prefix "${OUT}/run" \
    --microbench "${OUT}/sizes.json" --label "${LABEL}" \
    --steady-prefix "${OUT}/steady" --storage-md "${OUT}/storage.md" --out "${OUT}/report.md"

# A run that finishes without all its data is the failure mode that cost an overnight
# sweep its storage and ECC verify numbers, so fail loudly now naming what is missing
# rather than discovering the gap weeks later.
echo "=== collect raw data (before teardown deletes the node volumes) ==="
RAW="${OUT}/raw"
rm -rf "${RAW}"
mkdir -p "${RAW}/logs" "${RAW}/configs"
cp -r "${DEVNET_DIR}/perf" "${RAW}/perf" 2>/dev/null || true
cp "${DEVNET_DIR}/.env" "${RAW}/configs/devnet.env" 2>/dev/null || true
for d in "${DEVNET_DIR}"/configs/*/; do
    n=$(basename "${d}")
    mkdir -p "${RAW}/configs/${n}"
    cp "${d}validators.txt" "${RAW}/configs/${n}/" 2>/dev/null || true
    # DevNet-only validator tokens are dropped from the published copy.
    awk '/^\[validator_token\]/{skip=1; print; print "<redacted>"; next} /^\[/{skip=0} !skip' \
        "${d}xrpld.cfg" >"${RAW}/configs/${n}/xrpld.cfg" 2>/dev/null || true
done
for c in $(docker ps --format '{{.Names}}' | grep '^rippled-' | sort); do
    docker cp "${c}:/var/lib/rippled/debug.log" - 2>/dev/null | tar -xO 2>/dev/null |
        gzip >"${RAW}/logs/${c}.debug.log.gz" || true
    [[ -s "${RAW}/logs/${c}.debug.log.gz" ]] || rm -f "${RAW}/logs/${c}.debug.log.gz"
    curl -s -m 10 -d '{"method":"server_info","params":[{"counters":true}]}' \
        "http://127.0.0.1:$(docker port "${c}" 5005/tcp 2>/dev/null | head -1 | cut -d: -f2)" \
        >"${RAW}/logs/${c}.server_info.final.json" 2>/dev/null || true
done
docker compose -f "${COMPOSE}" ps >"${RAW}/compose_ps.txt" 2>&1 || true

echo "=== completeness check ==="
MISSING=()
grep -ql '"event"' "${DEVNET_DIR}"/perf/*/perf.log 2>/dev/null || MISSING+=("in-process probes")
[[ -s "${OUT}/sizes.json" ]] || MISSING+=("microbench/sizes")
if [[ ${MULTISIGN} -eq 1 ]]; then
    [[ -s "${OUT}/multisign.json" ]] || MISSING+=("multi-sign verify curve")
elif [[ ${MS_THROUGHPUT} -eq 1 ]]; then
    ls "${OUT}"/run_ms*.cl_summary.json >/dev/null 2>&1 || MISSING+=("multi-sign throughput")
elif [[ "${METHOD}" == "steady" ]]; then
    [[ -s "${OUT}/monitor.csv" ]] || MISSING+=("resource monitor")
    python3 -c "import json,sys;sys.exit(0 if json.load(open('${OUT}/steady.summary.json')).get('levels') else 1)" \
        2>/dev/null || MISSING+=("steady-state levels")
    [[ -s "${OUT}/steady.ledgers.jsonl" ]] || MISSING+=("per-ledger record")
    grep -q '"node":"validator-' "${OUT}/steady.server_info.jsonl" 2>/dev/null || MISSING+=("validator samples")
    [[ -s "${OUT}/steady.fee.jsonl" ]] || MISSING+=("queue/fee samples")
    ls "${OUT}"/steady.submits.p*.csv.gz >/dev/null 2>&1 || MISSING+=("per-submit generator log")
    [[ -s "${OUT}/storage.json" ]] || MISSING+=("storage breakdown")
    ls "${OUT}"/raw/logs/*.debug.log.gz >/dev/null 2>&1 || MISSING+=("node debug logs")
else
    [[ -s "${OUT}/monitor.csv" ]] || MISSING+=("resource monitor")
    python3 -c "import json,sys;sys.exit(0 if json.load(open('${OUT}/run.cl_summary.json')).get('levels') else 1)" \
        2>/dev/null || MISSING+=("throughput/latency levels")
    python3 -c "
import json, sys
base = set(json.load(open('${OUT}/disk_before.json'))['sizes_bytes'])
def healthy(sizes):
    return bool(base) and base <= set(sizes) and all(sizes[n] >= 0 for n in base)
ok = False
try:
    ok = ok or healthy(json.load(open('${OUT}/disk_after.json'))['sizes_bytes'])
except Exception:
    pass
try:
    ok = ok or any(healthy(lv.get('disk_bytes') or {}) for lv in json.load(open('${OUT}/run.cl_disk.json')))
except Exception:
    pass
sys.exit(0 if ok else 1)" 2>/dev/null || MISSING+=("storage growth")
fi

echo "=== teardown ==="
docker compose -f "${COMPOSE}" down -v >/dev/null 2>&1 || true

if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "RUN INCOMPLETE (${LABEL}): missing ${MISSING[*]}." >&2
    echo "The report under ${OUT}/ is partial; re-run this configuration." >&2
    exit 1
fi
echo "RUN COMPLETE (${LABEL}): all expected data captured. Artifacts in ${OUT}/"
