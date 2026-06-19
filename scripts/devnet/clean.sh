#!/usr/bin/env bash
# Tear down the hybrid-PQ DevNet and wipe its named volumes.
#
# `docker compose down` alone keeps the per-node data volumes around,
# which makes the next `up` boot against stale ledger state. That
# defeats the --start flag (which only pre-enables [amendments] on a
# fresh database), so Quantum stays in the normal 2-week voting cycle
# and bootstrap.py times out waiting for it to activate.
#
# Run this between runs, or use `docker compose down -v` directly.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

docker compose -f "${SCRIPT_DIR}/docker-compose.yml" down -v

# Clear the host-side perf log directories so a new run starts with empty
# PerfLog files (stale lines would otherwise pollute the aggregator).
rm -rf "${SCRIPT_DIR}/perf"

echo
echo "Network torn down, volumes and perf logs wiped. Next: bash ${SCRIPT_DIR}/setup.sh"
