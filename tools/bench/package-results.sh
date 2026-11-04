#!/usr/bin/env bash
# Package a results directory for publication.
#
# Usage: tools/bench/package-results.sh [RESULTS_DIR] [OUT_BASENAME]
#   (defaults: tools/bench/results  /tmp/bench-results)
#
# Produces OUT_BASENAME.tar.gz, OUT_BASENAME.SHA256SUMS (one hash per file
# inside the archive, plus the archive itself) and OUT_BASENAME.MANIFEST.txt.
# Excluded: the pre-signed corpora (large run inputs, regenerable) and the
# account secrets; accounts.json is replaced by accounts.public.json holding
# only addresses and (hybrid) PQ public keys.
#
# Publishing is a separate, deliberate step, e.g. a GitHub release asset:
#   gh release create bench-rerun-YYYYMMDD OUT.tar.gz OUT.SHA256SUMS OUT.MANIFEST.txt
set -euo pipefail
RES="${1:-tools/bench/results}"
OUTB="${2:-/tmp/bench-results}"
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT

mkdir -p "${STAGE}/results"
rsync -a --exclude='corpus*.jsonl' --exclude='corpus*.jsonl.gz' --exclude='accounts.json' \
    "${RES}/" "${STAGE}/results/"
find "${RES}" -name accounts.json | while read -r a; do
    rel="${a#"${RES}"/}"
    python3 - "${a}" "${STAGE}/results/$(dirname "${rel}")/accounts.public.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
pub = [{"account": r["account"], "pq_pubkey": r.get("pq_pubkey")} for r in d["accounts"]]
json.dump({"hybrid": d.get("hybrid"), "accounts": pub}, open(sys.argv[2], "w"))
PY
done
GIT_COMMIT="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
{
    echo "bench results package"
    echo "created_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "harness_commit: ${GIT_COMMIT}"
    echo
    (cd "${STAGE}" && find results -type f -exec ls -l {} \; | awk '{print $5"\t"$9}' | sort -k2)
} >"${OUTB}.MANIFEST.txt"
(cd "${STAGE}" && find results -type f -print0 | sort -z | xargs -0 sha256sum) >"${OUTB}.SHA256SUMS"
tar czf "${OUTB}.tar.gz" -C "${STAGE}" results
sha256sum "${OUTB}.tar.gz" | sed "s|${OUTB}.tar.gz|$(basename "${OUTB}").tar.gz|" >>"${OUTB}.SHA256SUMS"
echo "Packaged: ${OUTB}.tar.gz ($(du -h "${OUTB}.tar.gz" | cut -f1)), ${OUTB}.SHA256SUMS, ${OUTB}.MANIFEST.txt"
