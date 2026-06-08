#!/usr/bin/env bash
# End-to-end acceptance test for the pq-wallet-cli custody-flow demo.
#
# Launches rippled in standalone mode with the Quantum amendment enabled at
# genesis, then drives the PoC project description flow through the wallet
# binary: keygen, funding, opt-in, hybrid sign + submit, show-account, and
# a RIPPLE_CUSTODY_FAIL=1 negative path proving the server-side hybrid
# verification is what gates acceptance.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
XRPLD="${XRPLD:-${REPO_ROOT}/.build/xrpld}"
WALLET_BIN="${WALLET_BIN:-${REPO_ROOT}/.build/pq-wallet-cli}"
CFG="${REPO_ROOT}/tools/pq-wallet-cli/demo.cfg"
RPC_URL="http://127.0.0.1:5050"
DATA_DIR="/tmp/pq-wallet-cli-demo"
WALLET_PATH="/tmp/pq-wallet-cli-demo-wallet.json"
SIDECAR="/tmp/pq-wallet-cli-signed-tx.json"
GENESIS="rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"
GENESIS_SECRET="masterpassphrase"
FUND_DROPS="1000000000"
PAY_DROPS="100000000"

for tool in "$XRPLD" "$WALLET_BIN"; do
    if [[ ! -x "$tool" ]]; then
        echo "Required binary missing: $tool" >&2
        echo "Build with: cmake -Dpq_wallet_cli=ON --preset=conan-debug && cmake --build .build --target xrpld pq-wallet-cli -j$(nproc)" >&2
        exit 2
    fi
done

cleanup() {
    if [[ -n "${RIPPLED_PID:-}" ]] && kill -0 "${RIPPLED_PID}" 2>/dev/null; then
        kill "${RIPPLED_PID}" 2>/dev/null || true
        wait "${RIPPLED_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

rm -rf "$DATA_DIR" "$WALLET_PATH" "${WALLET_PATH%.json}.custody.json" \
    "${WALLET_PATH%.json}.pq.json" "$SIDECAR"
mkdir -p "$DATA_DIR/db"

echo "==> Launching rippled standalone (port 5050, Quantum pre-enabled)"
"$XRPLD" --conf "$CFG" --start -a >/dev/null 2>&1 &
RIPPLED_PID=$!

rpc() {
    local method="$1"
    local params="$2"
    curl -sS -H "Content-Type: application/json" -X POST \
        --data "{\"method\":\"${method}\",\"params\":[${params}]}" \
        "$RPC_URL"
}

# Poll until rippled accepts RPC.
echo "==> Waiting for rippled JSON-RPC"
for _ in $(seq 1 60); do
    if rpc server_info '{}' >/dev/null 2>&1; then
        break
    fi
    sleep 0.5
done
rpc ledger_accept '{}' >/dev/null

echo "==> Step 1: wallet keygen"
"$WALLET_BIN" keygen --wallet "$WALLET_PATH"
ACCOUNT="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['account_id'])" "$WALLET_PATH")"
echo "    wallet account: $ACCOUNT"

echo "==> Step 2: fund wallet account from genesis"
SIGNED="$(rpc sign "{\"secret\":\"${GENESIS_SECRET}\",\"tx_json\":{\"TransactionType\":\"Payment\",\"Account\":\"${GENESIS}\",\"Destination\":\"${ACCOUNT}\",\"Amount\":\"${FUND_DROPS}\"}}")"
BLOB="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['result']['tx_blob'])" "$SIGNED")"
rpc submit "{\"tx_blob\":\"${BLOB}\"}" >/dev/null
rpc ledger_accept '{}' >/dev/null

echo "==> Step 3: wallet opt-in (AccountSet asfQuantum)"
"$WALLET_BIN" opt-in --wallet "$WALLET_PATH" --rpc-url "$RPC_URL"
rpc ledger_accept '{}' >/dev/null

echo "==> Step 4a: wallet sign-tx (hybrid Payment to genesis)"
"$WALLET_BIN" sign-tx \
    --wallet "$WALLET_PATH" --rpc-url "$RPC_URL" --out "$SIDECAR" \
    --to "$GENESIS" --amount-drops "$PAY_DROPS" >/dev/null
echo "    sidecar written to $SIDECAR"

echo "==> Step 4b: wallet submit-tx"
"$WALLET_BIN" submit-tx --rpc-url "$RPC_URL" --in "$SIDECAR"
rpc ledger_accept '{}' >/dev/null

echo "==> Step 5: wallet show-account"
"$WALLET_BIN" show-account --wallet "$WALLET_PATH" --rpc-url "$RPC_URL"

echo "==> Step 6: negative path (RIPPLE_CUSTODY_FAIL=1 → expect rejection)"
RIPPLE_CUSTODY_FAIL=1 "$WALLET_BIN" sign-tx \
    --wallet "$WALLET_PATH" --rpc-url "$RPC_URL" --out "$SIDECAR" \
    --to "$GENESIS" --amount-drops "$PAY_DROPS" >/dev/null
# The mock corrupts the first byte of the DER ECDSA signature. The server
# rejects this either as `tefBAD_AUTH` (binding gate, if the DER still
# parses) or as `invalidTransaction` / `Invalid signature` (local-checks
# rejection at DER decode). Either outcome proves the wallet's acceptance
# in the happy path is gated by the server, not by the wallet itself.
NEG_OUT="$("$WALLET_BIN" submit-tx --rpc-url "$RPC_URL" --in "$SIDECAR" 2>&1)" || true
echo "$NEG_OUT"
if echo "$NEG_OUT" | grep -qE "tefBAD_AUTH|invalidTransaction|Invalid signature"; then
    echo "    negative path confirmed: server rejected the corrupted ECC signature"
else
    echo "    NEGATIVE PATH FAILED: server did not reject the corrupted signature" >&2
    exit 1
fi

echo
echo "==> Demo complete."
