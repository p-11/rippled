#!/usr/bin/env bash
# Generate per-node configs for the hybrid-PQ benchmark DevNet (5 validators +
# 3 stock nodes).
#
# Reproducible: every validator gets fixed master seeds, so re-running this
# script produces identical master identities (ephemeral keys are always fresh).
# Run once before `docker compose up`.
#
# PROFILE selects the benchmark configuration:
#   hybrid        (default) Quantum pre-enabled, fail_closed PQ validations
#   amendment-off hybrid binary, Quantum left disabled, no PQ validations
#   upstream      clean develop binary, ECC only (same empty sections as
#                 amendment-off; pair with XRPLD_BIN pointing at the upstream build)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
XRPLD="${XRPLD:-${REPO_ROOT}/.build/xrpld}"
CONFIGS_DIR="${SCRIPT_DIR}/configs"
PERF_DIR="${SCRIPT_DIR}/perf"
PROFILE="${PROFILE:-hybrid}"

QUANTUM_HASH="8BDE7A46A94C208C968B694659D99F89D5552A1F89F1879B7315AF2ADB3F390B"

if [[ ! -x "${XRPLD}" ]]; then
    echo "xrpld binary not found at ${XRPLD}" >&2
    echo "Build it with: cmake --build .build --target xrpld -j\$(nproc)" >&2
    exit 1
fi

# Resolve the profile-specific config sections.
case "${PROFILE}" in
    hybrid)
        AMENDMENTS_SECTION=$'[amendments]\n'"${QUANTUM_HASH} Quantum"
        PQ_VALIDATIONS_SECTION=$'[pq_validations]\nfail_closed'
        ;;
    amendment-off | upstream)
        AMENDMENTS_SECTION=""
        PQ_VALIDATIONS_SECTION=""
        ;;
    *)
        echo "Unknown PROFILE '${PROFILE}' (expected hybrid|amendment-off|upstream)" >&2
        exit 1
        ;;
esac

echo "Generating configs for PROFILE=${PROFILE} using ${XRPLD}"

# Deterministic master PQ seeds for the 5 validators (DevNet only -- DO NOT
# reuse these on any network with real value). Master ECC seeds are random
# per setup.sh run, so validators.txt is regenerated each time.
declare -a PQ_SEEDS=(
    "1111111111111111111111111111111111111111111111111111111111111111"
    "2222222222222222222222222222222222222222222222222222222222222222"
    "3333333333333333333333333333333333333333333333333333333333333333"
    "4444444444444444444444444444444444444444444444444444444444444444"
    "5555555555555555555555555555555555555555555555555555555555555555"
)

# Upstream profile uses the binary-independent [validation_seed] mechanism so the
# upstream develop binary (which lacks the hybrid token generator) runs the same
# network. These ECC validator identities are derived deterministically from
# fixed passphrases (validation_create on "devnet-validator-N"); DevNet only --
# DO NOT reuse on any network with real value.
declare -a UPSTREAM_VAL_SEEDS=(
    "snfK7iuZz3arEtb1UaYG3ypz3MABQ"
    "snCTHz3tUH78j7nWN5UTJ7rVy8kXQ"
    "shnoB9xHSzGrXfjYBUmSo9GE7krEd"
    "sh2Y3jkVFjKVv7F5m9KSPyBnoAeoN"
    "ssCCbvnDjfEHjkbjvYuv5kznuDKrt"
)
declare -a UPSTREAM_VAL_PUBKEYS=(
    "n9MzoUniAknf7CWX5GE6fFfHh9qhd2Sg7Pz5sKKiRZ6CUoXUzwZG"
    "n9M9C2WNUWAh9CbtFi3iXpA7LA2BWfdLX2bdke4698sFUYKdxM6i"
    "n9KL4u7e6bKT8nRcLXLDHhU4a5XLDckCQyfvCEQv373SEgTNqQzF"
    "n9McbFjNKh2xnm3nXKJrTBJmseSC3wPKhCfqaAgqGXSCHiysBfoj"
    "n9L1dXU6HPJS7C4v4v3M5eLDCvYPuHu2CQMwmQdmhwRpxSy66J5G"
)
mkdir -p "${CONFIGS_DIR}"

# Build the [ips_fixed] list once; every node uses it (rippled tolerates self).
IPS_FIXED=""
for i in 1 2 3 4 5; do
    IPS_FIXED+="rippled-validator-${i} 51235"$'\n'
done
for i in 1 2 3; do
    IPS_FIXED+="rippled-stock-${i} 51235"$'\n'
done
IPS_FIXED="${IPS_FIXED%$'\n'}"

# Render a template, substituting the shared placeholders. $1=template $2=output.
render() {
    sed -e "s|__IPS_FIXED__|${IPS_FIXED//$'\n'/\\n}|" \
        -e "s|__AMENDMENTS_SECTION__|${AMENDMENTS_SECTION//$'\n'/\\n}|" \
        -e "s|__PQ_VALIDATIONS_SECTION__|${PQ_VALIDATIONS_SECTION//$'\n'/\\n}|" \
        "$1" >"$2"
}

# Collect validator pubkeys for validators.txt.
VALIDATORS_TXT_BODY=""

for i in 1 2 3 4 5; do
    NODE_DIR="${CONFIGS_DIR}/validator-${i}"
    mkdir -p "${NODE_DIR}"
    mkdir -p "${PERF_DIR}/validator-${i}"

    if [[ "${PROFILE}" == "upstream" ]]; then
        # ECC-only validator identity via [validation_seed]; no token generator,
        # so this path makes no calls to xrpld and runs on the develop binary.
        VALIDATORS_TXT_BODY+="    ${UPSTREAM_VAL_PUBKEYS[$((i - 1))]}"$'\n'
        sed -e "s|__IPS_FIXED__|${IPS_FIXED//$'\n'/\\n}|" \
            -e "s|__VALIDATION_SEED__|${UPSTREAM_VAL_SEEDS[$((i - 1))]}|" \
            "${SCRIPT_DIR}/templates/validator-upstream.cfg.tmpl" \
            >"${NODE_DIR}/xrpld.cfg"
        continue
    fi

    echo "Generating token for validator-${i}..."
    "${XRPLD}" --generate-hybrid-validator-token \
        --master-pq-seed-hex "${PQ_SEEDS[$((i - 1))]}" \
        --domain "validator-${i}.devnet" \
        --sequence 1 \
        >"${NODE_DIR}/.token.b64" \
        2>"${NODE_DIR}/identity.txt"

    # The token is one giant base64 line; wrap to 72 chars per line so it's
    # readable inside [validator_token].
    TOKEN_WRAPPED=$(fold -w 72 <"${NODE_DIR}/.token.b64")

    MASTER_ECC_PK=$(grep "^# Master ECC public key:" "${NODE_DIR}/identity.txt" |
        awk '{print $NF}')

    VALIDATORS_TXT_BODY+="    ${MASTER_ECC_PK}"$'\n'

    sed -e "s|__IPS_FIXED__|${IPS_FIXED//$'\n'/\\n}|" \
        -e "s|__AMENDMENTS_SECTION__|${AMENDMENTS_SECTION//$'\n'/\\n}|" \
        -e "s|__PQ_VALIDATIONS_SECTION__|${PQ_VALIDATIONS_SECTION//$'\n'/\\n}|" \
        -e "s|__VALIDATOR_TOKEN__|${TOKEN_WRAPPED//$'\n'/\\n}|" \
        "${SCRIPT_DIR}/templates/xrpld.cfg.tmpl" \
        >"${NODE_DIR}/xrpld.cfg"

    rm -f "${NODE_DIR}/.token.b64"
done

# Stock node configs (no validator_token).
for i in 1 2 3; do
    STOCK_DIR="${CONFIGS_DIR}/stock-${i}"
    mkdir -p "${STOCK_DIR}"
    mkdir -p "${PERF_DIR}/stock-${i}"
    render "${SCRIPT_DIR}/templates/stock.cfg.tmpl" "${STOCK_DIR}/xrpld.cfg"
done

# Shared validators.txt -- every node trusts the same 5 master ECC keys.
VALIDATORS_TXT="[validators]"$'\n'"${VALIDATORS_TXT_BODY}"
for d in "${CONFIGS_DIR}"/validator-*/ "${CONFIGS_DIR}"/stock-*/; do
    printf '%s' "${VALIDATORS_TXT}" >"${d}validators.txt"
done

# Core pinning adapts to the host via a .env file Docker Compose auto-loads from
# this directory, so both this manual path and run-suite.sh get it. The compose
# cpuset map pins each node to a disjoint core subset and needs >=16 logical CPUs;
# on a smaller host that map would fail (a cpuset referencing a missing core) or
# starve the driver, so point every node at the full core range instead -- valid
# everywhere, just without the contention isolation.
NCPU="$(nproc)"
ENV_FILE="${SCRIPT_DIR}/.env"
if [[ ${NCPU} -lt 16 ]]; then
    ALL="0-$((NCPU - 1))"
    echo "Host has ${NCPU} logical CPUs (<16): disabling exclusive core pinning."
    {
        for v in 1 2 3 4 5; do echo "DEVNET_CPUSET_VALIDATOR_${v}=${ALL}"; done
        for s in 1 2 3; do echo "DEVNET_CPUSET_STOCK_${s}=${ALL}"; done
        echo "DEVNET_PINNING_NOTE=\"unpinned: host has ${NCPU} logical CPUs, all nodes share ${ALL}\""
    } >"${ENV_FILE}"
elif [[ ${NCPU} -eq 16 ]]; then
    echo 'DEVNET_PINNING_NOTE="validators 0-9 (2 each), stock 10-12, host/driver 13-15"' >"${ENV_FILE}"
else
    # On a bigger host (e.g. a bare-metal benchmark instance) scale the disjoint
    # cpuset map up: give each validator and stock node a larger contiguous core
    # slice and reserve the top cores for the driver, monitor, and OS. Without
    # this, every host >16 CPUs would reuse the 16-core map and waste the rest.
    reserve=$((NCPU / 6))
    ((reserve < 4)) && reserve=4 # driver / monitor / OS
    avail=$((NCPU - reserve))
    share=$((avail / 13))
    ((share < 1)) && share=1 # validators weight 2, stock 1
    vc=$((share * 2))
    sc=${share}
    rng() { (($2 > $1)) && echo "$1-$2" || echo "$1"; }
    cur=0
    {
        for v in 1 2 3 4 5; do
            echo "DEVNET_CPUSET_VALIDATOR_${v}=$(rng ${cur} $((cur + vc - 1)))"
            cur=$((cur + vc))
        done
        for s in 1 2 3; do
            echo "DEVNET_CPUSET_STOCK_${s}=$(rng ${cur} $((cur + sc - 1)))"
            cur=$((cur + sc))
        done
        echo "DEVNET_PINNING_NOTE=\"validators ${vc} cores each, stock ${sc} each," \
            "host/driver $(rng ${cur} $((NCPU - 1))) (${NCPU} CPUs)\""
    } >"${ENV_FILE}"
    echo "Scaled core pinning for ${NCPU} CPUs: validators ${vc} cores each, stock ${sc} each."
fi

echo
echo "Generated configs under ${CONFIGS_DIR}/ and perf dirs under ${PERF_DIR}/"
echo "Next: docker compose -f ${SCRIPT_DIR}/docker-compose.yml up --build"
