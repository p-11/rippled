#!/usr/bin/env bash
# One consistent benchmark batch across all configs, for report regeneration.
#
# Throughput, latency, and CPU are run-to-run noisy, so the three main configs
# are run --repeats times with distinct labels (results/<config>-rN). The ECC and
# hybrid configs share one rate band: it only has to bracket the highest knee, so
# the wider ECC band covers both and gives the lower hybrid knee corpus headroom.
#
# The single-sign throughput configs use the steady-state method from the
# RippleX review (run-suite.sh --method steady): xrpld default queue settings,
# ramp / settle / measure per level, pass while every validated-ledger interval
# stays <= 5 s, throughput from the ledgers, account pool sized automatically.
# summarize_repeats.py then checks the repeats agree within one load step.
#
# Usage:
#   tools/bench/run-sweep.sh [--repeats N] [--pool N|auto] [--measure S]
set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}" || exit 1

REPEATS=3
POOL=auto
MEASURE=120
# One rate band for every single-sign throughput config. Each level is held to
# steady state, so the run stops at the first level that breaks the 5 s
# criterion; cap is only a ceiling, rate-expected sizes the pre-signed corpus.
RATE=(--rate-start 250 --rate-step 250 --rate-cap 4000 --rate-expected 3000)
while [[ $# -gt 0 ]]; do
    case "$1" in
        --repeats)
            REPEATS="$2"
            shift 2
            ;;
        --pool)
            POOL="$2"
            shift 2
            ;;
        --measure)
            MEASURE="$2"
            shift 2
            ;;
        -h | --help)
            echo "Usage: tools/bench/run-sweep.sh [--repeats N] [--pool N|auto] [--measure S]"
            exit 0
            ;;
        *)
            echo "run-sweep: unknown arg '$1'" >&2
            exit 1
            ;;
    esac
done

COMMON=(--method steady --pool "${POOL}" --measure "${MEASURE}")

LOG="/tmp/bench-sweep.log"
: >"${LOG}"
rm -f /tmp/bench-sweep.done

INCOMPLETE=()

run() { # run <label> <run-suite args...>
    local label="$1"
    shift
    echo "=================== START ${label} ($(date -u +%H:%M:%SZ)) ===================" | tee -a "${LOG}"
    bash tools/bench/run-suite.sh "$@" >>"${LOG}" 2>&1
    local rc=$?
    # run-suite exits non-zero when its completeness check found missing data.
    [[ ${rc} -ne 0 ]] && INCOMPLETE+=("${label}")
    echo "=================== END ${label} exit=${rc} ($(date -u +%H:%M:%SZ)) ===================" | tee -a "${LOG}"
}

for r in $(seq 1 "${REPEATS}"); do
    run "upstream-r${r}" --profile upstream --label "upstream-r${r}" "${COMMON[@]}" "${RATE[@]}"
    run "amendment-off-r${r}" --profile amendment-off --label "amendment-off-r${r}" "${COMMON[@]}" "${RATE[@]}"
    run "hybrid-r${r}" --profile hybrid --label "hybrid-r${r}" "${COMMON[@]}" "${RATE[@]}"
done

# AVX2 variants run once each: the AVX2 backend only shifts the hybrid verify
# cost, so n=1 is enough to place the knee.
run "amendment-off-avx2" --profile amendment-off --avx2 --label amendment-off-avx2 "${COMMON[@]}" "${RATE[@]}"
run "hybrid-avx2" --profile hybrid --avx2 --label hybrid-avx2 "${COMMON[@]}" "${RATE[@]}"

# Multi-sign throughput-by-N is intentionally not swept: it adds nothing the
# single-sign throughput plus the per-signer payload delta do not already show,
# and a meaningful knee needs deep per-N corpora.
run "multisign" --profile hybrid --multisign --label multisign
# ECC baseline for the multi-sign per-signer N-curve, on the upstream develop
# binary, so the report's ECC-vs-hybrid table is reproducible from the sweep alone
# (the upstream-rN runs above already built the develop baseline this reuses).
run "multisign-ecc" --profile upstream --multisign --label multisign-ecc

if [[ ${#INCOMPLETE[@]} -gt 0 ]]; then
    echo "SWEEP INCOMPLETE: these runs are missing data and should be re-run: ${INCOMPLETE[*]}" | tee -a "${LOG}"
else
    echo "ALL runs captured their data." | tee -a "${LOG}"
fi
echo "ALL_SWEEP_DONE repeats=${REPEATS} ($(date -u +%H:%M:%SZ))" | tee -a "${LOG}"
touch /tmp/bench-sweep.done

python3 tools/bench/summarize_repeats.py --results tools/bench/results \
    --baseline upstream --compare hybrid --out tools/bench/results/sweep_summary.md | tee -a "${LOG}"

# Package the raw data for publication (corpora and account secrets excluded).
bash tools/bench/package-results.sh tools/bench/results /tmp/bench-results | tee -a "${LOG}"
