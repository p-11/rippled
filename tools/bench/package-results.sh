#!/usr/bin/env bash
# Package a results directory for publication.
#
# Usage: tools/bench/package-results.sh [RESULTS_DIR] [OUT_BASENAME]
#   (defaults: tools/bench/results  /tmp/bench-results)
#
# Produces OUT_BASENAME.tar.gz, OUT_BASENAME.SHA256SUMS (one hash per file
# inside the archive, plus the archive itself) and OUT_BASENAME.MANIFEST.txt.
# Excluded: the pre-signed corpora (large run inputs that can be re-signed) and the
# account secrets; each accounts.json gets an accounts.public.json beside it
# holding only addresses and (hybrid) PQ public keys, which is packaged instead.
# Also excluded unless PACKAGE_FULL=1: the per-node debug logs (*.debug.log.gz)
# and perf profiles (perf.log), several GB per node per run. They are listed
# with their sizes in the MANIFEST under "not packaged".
#
# The archive is written straight from RESULTS_DIR, with no staging copy, so a
# results set of tens of GB needs no extra free space beyond the archive itself.
# Put OUT_BASENAME on a disk with room for it (not /tmp on a small root disk).
#
# Publishing is a separate, deliberate step, e.g. a GitHub release asset:
#   gh release create bench-rerun-YYYYMMDD OUT.tar.gz OUT.SHA256SUMS OUT.MANIFEST.txt
set -euo pipefail
RES="${1:-tools/bench/results}"
OUTB="${2:-/tmp/bench-results}"
RES="$(cd "${RES}" && pwd)"
PARENT="$(dirname "${RES}")"
NAME="$(basename "${RES}")"

find "${RES}" -name accounts.json | while read -r a; do
    python3 - "${a}" "$(dirname "${a}")/accounts.public.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
pub = [{"account": r["account"], "pq_pubkey": r.get("pq_pubkey")} for r in d["accounts"]]
json.dump({"hybrid": d.get("hybrid"), "accounts": pub}, open(sys.argv[2], "w"))
PY
done

# The files that go in the archive, in a stable order (NUL-separated).
LIST="$(mktemp)"
trap 'rm -f "${LIST}"' EXIT
HEAVY=()
if [[ "${PACKAGE_FULL:-0}" != 1 ]]; then
    HEAVY=(! -name '*.debug.log.gz' ! -name perf.log)
fi
(cd "${PARENT}" && find "${NAME}" -type f ! -name accounts.json \
    ! -name 'corpus*.jsonl' ! -name 'corpus*.jsonl.gz' "${HEAVY[@]}" -print0 | sort -z) >"${LIST}"

GIT_COMMIT="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
{
    echo "bench results package"
    echo "created_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "harness_commit: ${GIT_COMMIT}"
    echo
    (cd "${PARENT}" && xargs -0 ls -l <"${LIST}" | awk '{print $5"\t"$9}')
    if [[ "${PACKAGE_FULL:-0}" != 1 ]]; then
        echo
        echo "not packaged (debug logs and perf profiles; kept with the run host):"
        (cd "${PARENT}" && find "${NAME}" -type f \( -name '*.debug.log.gz' -o -name perf.log \) \
            -printf '%s\t%p\n' | sort -k2)
    fi
} >"${OUTB}.MANIFEST.txt"
(cd "${PARENT}" && xargs -0 sha256sum <"${LIST}") >"${OUTB}.SHA256SUMS"
(cd "${PARENT}" && tar czf "${OUTB}.tar.gz" --null -T "${LIST}")
sha256sum "${OUTB}.tar.gz" | sed "s|${OUTB}.tar.gz|$(basename "${OUTB}").tar.gz|" >>"${OUTB}.SHA256SUMS"
echo "Packaged: ${OUTB}.tar.gz ($(du -h "${OUTB}.tar.gz" | cut -f1)), ${OUTB}.SHA256SUMS, ${OUTB}.MANIFEST.txt"
